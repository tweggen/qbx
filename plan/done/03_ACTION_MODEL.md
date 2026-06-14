# Strategy: SAction — Unified Command Model

## Objective

Introduce a single command object — `SAction` — that serves three roles at once:

1. **GUI→engine handoff.** Every user-driven project mutation is enqueued as an `SAction` and drained by the audio engine at safe moments, decoupling UI responsiveness from audio-thread timing.
2. **Undo/redo unit.** Each applied action produces its inverse, which the engine ships back to the GUI for the undo stack.
3. **Scripting vocabulary.** Action names form the verb set of a future scripting language; XML serialization of the same actions doubles as the script's wire format.

The driving motivation is the classic DAW-during-playback pathology: under load, hasty UI clicks compete with audio rendering on the same data, causing glitches or lost edits on save. Making the action queue a first-class concept — including pending actions in the save artifact — converts "save during busy" from a race condition into a snapshot operation.

## Non-goals (for this proposal)

- Event sourcing: project state is canonical, not derived from an action log.
- A scripting language itself — only the substrate it would later sit on.
- Nested/composite actions as a base-class feature — deferred; scripting language will subsume most uses.
- A multi-user / shared-document model.

## Design

### Core interface

`SAction` objects are **immutable after construction**. They carry forward parameters only. The inverse is constructed fresh by `apply()` from the pre-mutation project state — no thread-shared mutable fields on the action itself.

```cpp
// smaragd/main/include/saction.h
class SProject;
class SAction;

struct SApplyResult {
    bool      applied;    // did the mutation succeed?
    SAction  *inverse;    // ownership transferred; null iff !applied or non-undoable
};

class SAction {
public:
    virtual ~SAction() = default;

    // Stable identity. Used as XML tag, scripting verb, registry key.
    virtual QString name() const = 0;

    // Per-subclass schema version. Bumped when writeXml format changes.
    virtual int     formatVersion() const { return 1; }

    // Engine-thread. Mutates the project AND constructs the inverse
    // from the pre-mutation state it just observed.
    //
    // applied=false → precondition failed (target gone, value rejected);
    // engine logs and continues. inverse may be null even on applied=true
    // for explicitly non-undoable actions (transport, etc.).
    virtual SApplyResult apply( SProject *project ) = 0;

    // Forward parameters only. Inverse is reconstructed at apply time.
    virtual void writeXml( QDomElement &elem ) const = 0;
    virtual bool readXml( const QDomElement &elem, int version ) = 0;

    // Coalescing at enqueue time. Same key + successful mergeWith
    // collapses two queued actions before the engine sees either.
    virtual QString mergeKey() const { return QString(); }
    virtual bool    mergeWith( const SAction * /*later*/ ) { return false; }
};
```

### Registry (string → action)

```cpp
class SActionRegistry {
public:
    using Factory = std::function<SAction*()>;
    static SActionRegistry &instance();

    void registerType( const QString &name, Factory f );
    SAction *create( const QString &name ) const;
    SAction *createFromXml( const QDomElement &elem ) const;  // reads name + version
    QStringList knownNames() const;                            // script introspection
};

// Each action TU self-registers:
//   static const bool s_reg = (SActionRegistry::instance().registerType(
//       "set-track-volume", []{ return new SSetTrackVolumeAction; }), true);
```

### Queue with cancel and enqueue-time merge

```cpp
class SActionQueue {
public:
    // GUI thread. Returns id used by tryCancel. May merge into queue tail.
    quint64 enqueue( SAction *action );

    // Engine thread. Returns null when empty.
    SAction *dequeue( quint64 *outId );

    // GUI thread. true → action was removed before engine drained it.
    // false → already drained; fall back to inverse-via-history.
    bool tryCancel( quint64 id );

    // For save — deep-copies pending actions in order.
    QList<SAction*> snapshotPending() const;

private:
    struct Entry { quint64 id; SAction *action; };
    mutable QMutex mtx_;
    QList<Entry>   q_;
    quint64        nextId_ = 1;
};
```

`enqueue` checks if the tail entry has the same `mergeKey()` and asks the tail to absorb the newcomer via `mergeWith`. On absorption the tail keeps its id and the newcomer is deleted. The caller's id (= tail's id) remains valid for `tryCancel`.

### History — defer-push undo

```cpp
class SActionHistory : public QObject {
    Q_OBJECT
public:
    SActionHistory( SActionQueue *q, QUndoStack *stack, QObject *parent );

    // Bound to Ctrl+Z. Two-tier lookup: in-flight first, then undo stack.
    void undo();
    void redo();

    // Called from GUI gesture handler.
    void submit( SAction *forward );

public slots:
    // Engine reports back after drain+apply (queued connection across threads).
    void onApplied(  quint64 id, SAction *inverse );  // inverse ownership → us
    void onRejected( quint64 id, const QString &reason );

private:
    struct InFlight { quint64 id; SAction *forward; };
    QList<InFlight>  inFlight_;   // FIFO, GUI-thread access only
    SActionQueue    *queue_;
    QUndoStack      *stack_;
};
```

`SActionHistory::undo()` walks the in-flight list newest-first and attempts `queue_->tryCancel(id)` for the latest entry. On success the entry is dropped without ever reaching the engine. On failure (engine already drained it) the call falls through to `stack_->undo()`. The Edit→Undo `QAction` is bound to `SActionHistory::undo` — never directly to `QUndoStack::undo`.

`onApplied(id, inverse)` removes the in-flight entry, then pushes a paired `SActionUndoCommand(forward, inverse)` onto the `QUndoStack`. Only then is the action visible in the standard Qt undo machinery (dirty tracking, QUndoView, etc.).

### Bridge to QUndoStack

```cpp
class SActionUndoCommand : public QUndoCommand {
public:
    SActionUndoCommand( SAction *forward, SAction *backward, SActionHistory *h );
    void redo() override { history_->submit( clone( forward_ ) ); }
    void undo() override { history_->submit( clone( backward_ ) ); }
    int  id() const override;                                 // hashed from mergeKey()
    bool mergeWith( const QUndoCommand *other ) override;     // delegates to SAction::mergeWith
};
```

Redo and undo both go back through `submit` → queue → engine apply. Inverse-of-inverse is symmetric; no special-casing.

### Save format (inline)

```xml
<project version="2">
  <data> … existing SProject content … </data>
  <pending-actions>
    <set-track-volume version="1" track="3" volume="0.5"/>
    <add-clip         version="1" track="3" sample="kick.wav" at="0.0"/>
  </pending-actions>
</project>
```

- Load: parse `<data>` as today, then iterate `<pending-actions>` children, use `SActionRegistry::createFromXml` on each, enqueue them. They drain on the engine thread same as live actions.
- Project file `version="2"` is the project-file version, distinct from per-action `version`. Existing files load as v1 (no `<pending-actions>` block) and re-save as v2 (block present, possibly empty).
- The pending block is snapshotted via `SActionQueue::snapshotPending()` at save start. Save never blocks waiting for the queue to drain.

### Selection as first-class state

Selection is treated as heavyweight project state, not view ephemera (Photoshop pattern):

- Lives in `SProject` (e.g. `SSelection currentSelection_`).
- Mutated only via SActions: `SSelectClipsAction`, `SDeselectAllAction`, `SInvertSelectionAction`, `SSelectByCriteriaAction`.
- Serialized in `<data>` with the rest of the project.
- Participates in undo (Step Backward walks selection changes too).
- Scriptable: `select(track=3, clips=[…])` is a registry verb.

UI-only state stays UI-only: hover, cursor position, scroll/zoom, current tool, transport playhead, focus widget. None mutate the project, none go through the queue.

### Versioning policy

- Every SAction subclass overrides `formatVersion()`. Bumped when `writeXml` output changes.
- `readXml(elem, version)` receives the version stored in the XML and is responsible for migration. Subclasses that never change their format ignore the parameter.
- Project file format gets its own root `version` attribute, incremented when the overall file structure changes (e.g. introduction of `<pending-actions>` block = v1 → v2).

## Example concrete action

```cpp
class SSetTrackVolumeAction : public SAction {
    int     trackIdx_  = -1;
    double  newVolume_ = 0.0;
public:
    SSetTrackVolumeAction() = default;
    SSetTrackVolumeAction( int t, double v ) : trackIdx_(t), newVolume_(v) {}

    QString name() const override { return QStringLiteral("set-track-volume"); }

    SApplyResult apply( SProject *p ) override {
        STrack *t = p->track( trackIdx_ );
        if( !t ) return { false, nullptr };
        double oldVolume = t->volume();
        t->setVolume( newVolume_ );
        return { true, new SSetTrackVolumeAction( trackIdx_, oldVolume ) };
    }

    QString mergeKey() const override {
        return QStringLiteral("set-track-volume:%1").arg( trackIdx_ );
    }
    bool mergeWith( const SAction *later ) override {
        auto *o = dynamic_cast<const SSetTrackVolumeAction*>( later );
        if( !o || o->trackIdx_ != trackIdx_ ) return false;
        newVolume_ = o->newVolume_;
        return true;
    }

    void writeXml( QDomElement &e ) const override {
        e.setAttribute( "track",  trackIdx_  );
        e.setAttribute( "volume", newVolume_ );
    }
    bool readXml( const QDomElement &e, int /*version*/ ) override {
        trackIdx_  = e.attribute("track").toInt();
        newVolume_ = e.attribute("volume").toDouble();
        return true;
    }
};
```

## Threading summary

| Thread | Responsibilities |
|---|---|
| GUI | Construct SAction forwards, submit to `SActionHistory`, handle `tryCancel`, receive `onApplied`/`onRejected`, manage `QUndoStack`, render UI from project state |
| Engine | Drain queue, call `apply()` on project, emit `onApplied`/`onRejected` back to GUI via queued signals |
| Audio render | Reads project state during buffer callback — never writes |

Project mutation is single-writer (engine thread). GUI reads project state for display; this needs read consistency but not write coordination. Existing `twStreamingLatch` ring buffers already handle the audio-render side of this. The engine's apply step happens between audio callbacks, in the same thread that drains the latches.

## Open items (intentionally deferred)

- **Composite actions as a base-class feature** — wait for scripting language; macros via `QUndoStack::beginMacro` cover ad-hoc UI gestures in the meantime.
- **Per-action migration framework** — postponed until the first action's format actually changes. `version` attribute is the hook.
- **Long-term action log persistence (for audit, replay, time-travel debugging)** — out of scope. The pending-actions block in the save file is the only persisted action data.
- **Cross-document selection / multi-user implications** of selection-as-state — flagged for future consideration.

## Phased rollout

1. **Substrate.** `SAction`, `SActionRegistry`, `SActionQueue`, `SActionHistory`, `SActionUndoCommand`. No subclasses yet. Wire the queue between GUI and engine on an empty schedule.
2. **First real action.** `SSetTrackVolumeAction` end-to-end: GUI slider submits, engine applies, inverse comes back, undo works, save/load round-trips with pending block.
3. **Inventory and port existing mutations.** Walk through current direct-mutation sites (track add/remove, sample import, mixer changes, clip placement) and convert each to an SAction subclass. Selection moves into the project at this stage.
4. **Format version bump.** Project files become v2 with the `<pending-actions>` block. v1 → v2 reader path is trivial (no block = empty pending queue).
5. **Scripting language pick & integration** (separate proposal). Once the action vocabulary is stable, choose a language (Lua / QJSEngine / custom DSL) and bind it to the registry.

## Acceptance criteria for phase 1+2

- Volume drag on a track produces audible change with no audio glitch under heavy CPU load.
- Ctrl+Z immediately after a drag undoes it even when audio is currently playing.
- Ctrl+Z during a burst of 20 rapid clicks correctly unwinds the most recent click — whether it was queued or already applied.
- Save during playback completes without dropouts; reload restores the same state including any pending-but-unapplied actions.
- Forced apply failure (e.g. delete a track between submit and drain) is logged and surfaced; UI state reconciles.
