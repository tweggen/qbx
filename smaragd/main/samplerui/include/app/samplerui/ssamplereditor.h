#ifndef SSAMPLEREDITOR_H
#define SSAMPLEREDITOR_H

// ssamplereditor.h — NassauDigital's editor window (proposal 56 §4.11, Q7;
// nassau-digital DESIGN §7, §9.3; QBX-279). The contract is
// samplerui/CONTRACT.md.
//
// ONE WINDOW PER INSTANCE, NOT A DOCK. Opened from the FX strip's edit action
// through pluginui's app-editor registry (spluginappeditors.h), which this
// module registers into from a static initializer. Like the native plugin
// window it is keyed by SLOT in a module-level registry and holds the TRACK
// and the SLOT, never a cached path: every submit derives trackPath (qualified
// by the root that holds the track) and slotIndex afresh, because both are
// positions that move under a window that outlives the strip. The slot's
// `editorOpen` flag says whether it is open, so a project saved with it open
// opens with it open.
//
// FOUR VIEWS AT FIXED SIZES (DESIGN §7): Key (a keyboard with the zones drawn
// on it), Pad (a 4×4 grid and a six-bank switcher), Map (key × velocity, every
// zone a rectangle, velocity gaps marked) and Zone (the selected zones' knob
// rows). Key, Pad and Map DISPLAY AND SELECT ONLY (the author, 2026-11-03);
// edits go through the Zone view's knobs, the XML pane and drag and drop. A
// view switch submits `set-sampler-view`, so it is undoable and saved with
// the instance.
//
// HAND SELECTION (A12; the author's answers of 2026-11-03): while the window
// is open it holds its OWN sink of the track's device MIDI input port (a
// second private ring on SMidiInputHub's fan-out; the engine's live source and
// the shell's key taps keep theirs). A key held selects the zones covering it,
// several held together select all of theirs; the selection stays after the
// keys are released until another key is held. It sets the selection DIRECTLY
// (SSamplerProgram::setSelection), like a device note's pad selection: not an
// undo step, not saved.
//
// THE XML COMMAND PANE (A5, DESIGN §9.3): one verb element, completed from the
// verbs' declared attributes (SAction::knownAttributes()); `trackPath` and
// `slotIndex` are implied; Apply submits ONE action, so one undo step.
//
// DRAG AND DROP of `application/x-smaragd-resource` (`file:` and `asset:`) onto
// a key, a pad or a zone; see dropResource().
//
// Every seam below exists so a headless case can drive the REAL widget and
// read back what it shows (the arranger-menu precedent): nothing here is ever
// asserted through geometry or a grab.

#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <cstdint>

#include "app/objects/sampler/ssamplerprogram.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strack.h"
#include "tw/devices/midi_in_fanout.h"

class QButtonGroup;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTimer;
class SAction;
class SPluginSlot;
class SProject;
class STrack;
class SSamplerKeyView;
class SSamplerMapView;
class SSamplerPadView;
class SSamplerZoneView;
class SSamplerXmlPane;

class SSamplerEditor : public QWidget {
    Q_OBJECT

public:
    using View = SSamplerProgram::View;

    // Open (or raise) the editor of `slot`. `parentForPosition`'s window()
    // owns it (the main window: a strip is deleted on every track switch).
    // Null when the slot does not host NassauDigital.
    static SSamplerEditor *openFor( STrack *track, SPluginSlot *slot,
                                    QWidget *parentForPosition, bool showWindow );
    static SSamplerEditor *editorFor( SPluginSlot *slot );
    static bool isOpenFor( SPluginSlot *slot );
    static void closeFor( SPluginSlot *slot );

    ~SSamplerEditor() override;

    // --- what the window is looking at -------------------------------------
    SPluginSlot     *slot() const { return slot_.data(); }
    STrack          *track() const { return track_.data(); }
    // The instance's program object, or null when it has none yet.
    SSamplerProgram *program() const;
    // The view shown: the program's, else the per-user default for a new
    // instance (ssamplerwiring::newInstanceView).
    View             view() const;
    // Derived at every call, never cached (see the header comment).
    QString currentTrackPath() const;
    int     currentSlotIndex() const;

    // Submit `xml` (one verb element) with the address implied, as the XML
    // pane and every gesture of this window do. `gesture` stamps the action
    // (a knob drag). True when the action was applied; false and `why` when it
    // could not be built or was refused.
    bool submitVerbXml( const QString &xml, quint64 gesture, QString *why );

    // --- THE SEAMS (samplerui/CONTRACT.md; the testkit's sampler-editor and
    // assert-sampler-editor verbs) ------------------------------------------
    // One line, `|`-separated fields, APPENDED to and never inserted into:
    //   open=1|view=<v>|page=<v>|pageSize=<W>x<H>|pageFixed=<0|1>|
    //   sizes=key:<W>x<H>,pad:…,map:…,zone:…|fixedAll=<0|1>|program=<0|1>|
    //   bank=<b>|pad=<p>|padKeys=<16 keys or ->|selected=<ids ;>|
    //   held=<keys ;>|tap=<0|1>|zones=<id:kLo-kHi/vLo-vHi;…>|
    //   gaps=<id:lo-hi,…;…>|zoneShown=<id|->|knobs=<id=value,…>|
    //   completions=<a,b,…>|xml=<pane text>|xmlStatus=<text>|undoIndex=<n>|
    //   title=<window title>
    QString describe() const;

    // Click a view button: "key" | "pad" | "map" | "zone".
    bool clickView( const QString &view );
    // Click a bank button (0..5) / a pad cell (0..15) of the Pad view.
    bool clickBank( int bank );
    bool clickPad( int pad );
    // A REAL mouse press on the Key view at `key`, on the Map view at
    // (`key`, `vel`): both select (select-zones) and nothing else.
    bool clickKey( int key );
    bool clickMap( int key, int vel );
    // A knob of the Zone view, by id ("zone.cutoff", "layer1.level"):
    // press, move to `value` (in the parameter's units), release — the
    // QAbstractSlider's own press/release (setSliderDown), so the gesture
    // token is minted and dropped by the production slots.
    bool knobPress( const QString &id );
    bool knobSet( const QString &id, double value );
    bool knobRelease( const QString &id );
    // The XML pane: put `text` in the editor with the cursor at `cursor`
    // (-1 = the end) and run the completion the typing runs; accept one of the
    // offered completions; press Apply.
    bool xmlType( const QString &text, int cursor );
    bool xmlAcceptCompletion( const QString &choice );
    bool xmlApply();
    // Drop `payload` (`file:<path>` | `asset:<name>` | `media:<uri>`) on
    // `target`: "key:<k>" (Key view), "pad:<p>" (Pad view, visible bank),
    // "zone" (the zone the Zone view shows), "map:<k>:<v>" (a zone rectangle
    // of the Map view). A real QDropEvent at the target's position in the
    // view, so the view's own hit test is on the path. False when nothing
    // was submitted (refused, or not accepted).
    bool dropResource( const QString &payload, const QString &target );
    // Drain the hand-selection tap now (what the poll does). Returns how many
    // messages it took.
    int  pollTap();

    // --- for the views --------------------------------------------------------
    QSet<int>     heldKeys() const { return held_; }
    QVector<std::uint32_t> selectedZones() const;
    QString       statusText() const;
    void          setStatus( const QString &text );
    // The drop rule (dropResource), as the views call it with a resolved
    // target: `key` >= 0 for a key or a pad's key, else `zoneId` != 0.
    bool          dropOn( const QString &payload, int key, std::uint32_t zoneId,
                          QString *why );

    // The window's views' fixed sizes (DESIGN §7: "fixed-size views").
    static QSize keyViewSize();
    static QSize padViewSize();
    static QSize mapViewSize();
    static QSize zoneViewSize();

    static QString viewName( View v );

protected:
    void closeEvent( QCloseEvent *e ) override;

private slots:
    void refresh();
    void syncTap();

private:
    SSamplerEditor( STrack *track, SPluginSlot *slot, QWidget *parent );
    void bindProgram_();
    void showView_( View v );
    void releaseTap_();
    void applyHandSelection_();
    QWidget *pageFor_( View v ) const;
    QWidget *knobById_( const QString &id ) const;

    QPointer<STrack>          track_;
    QPointer<SPluginSlot>     slot_;
    QPointer<SSamplerProgram> boundProgram_;
    QPointer<SProject>        project_;

    QButtonGroup     *viewButtons_ = nullptr;
    QPushButton      *viewButton_[4] = {};
    QStackedWidget   *stack_ = nullptr;
    SSamplerKeyView  *keyView_ = nullptr;
    SSamplerPadView  *padView_ = nullptr;
    SSamplerMapView  *mapView_ = nullptr;
    SSamplerZoneView *zoneView_ = nullptr;
    SSamplerXmlPane  *xmlPane_ = nullptr;
    QLabel           *status_ = nullptr;

    // THE TAP (hand selection): this window's own sink, held while it is open.
    audio::MidiInFanout       *fanout_ = nullptr;
    audio::MidiInFanout::Sink *sink_   = nullptr;
    QString                    tapPort_;
    int                        tapChannel_ = -1;
    QTimer                    *tapTimer_ = nullptr;
    bool                       closed_ = false;
    QSet<int>                  held_;
    QVector<int>               chord_;   // keys held since the last all-up, in order
};

#endif
