#ifndef SPLUGINEFFECTSTRIP_H
#define SPLUGINEFFECTSTRIP_H

#include <QString>
#include <QWidget>
#include <cstdint>
#include <memory>
#include <vector>

class SPluginSlot;
class SPluginChain;
class SPluginParamEditor;
class STrack;
class QDialog;
class QMenu;
class QVBoxLayout;
class QBoxLayout;
class QCheckBox;
class QPushButton;
class QLabel;
class QDragMoveEvent;
class QDropEvent;

// FX strip widget for a track. Shows the ordered list of plugins with:
// - Bypass toggle for each                (set-plugin-bypass action)
// - Edit button / double-click            (opens SPluginParamEditor)
// - Reload button on a Missing/Unsupported slot   (SPluginSlot::reloadPlugin())
// - Remove button for each                (remove-plugin action)
// - Add button to insert new plugins      (insert-plugin action)
// - Drag-to-reorder                       (reorder-plugin action)
// - A PER-ROW CONTEXT MENU carrying Edit, Bypass, Reload and Remove
//   (QBX-201), which is the only route to Remove and Reload in COMPACT mode
//   and is what stops a right-click aimed at a plugin reaching the MIXER
//   STRIP's own track menu, whose first item is "Remove track".
//
// proposal 08 M5. Every model mutation here goes through an ACTION
// (app/pluginui/CONTRACT.md invariant 3); before this milestone the bypass
// checkbox called SPluginSlot::setBypass() and the drop handler called
// SPluginChain::reorderSlot() directly, so neither was undoable.
class SPluginEffectStrip : public QWidget {
    Q_OBJECT
public:
    SPluginEffectStrip( STrack *track, QWidget *parent = nullptr );
    ~SPluginEffectStrip();

    // Open (or re-raise) the plugin window for one slot: the native GUI when
    // the plugin has an embeddable one, the generic parameter editor
    // otherwise. This is what a double-click on the row, the row's "Edit"
    // button and the row menu's Edit item all do.
    //
    // IT SHOWS NOTHING UNDER --test-case, and that is load-bearing rather
    // than tidy -- the same rule, for the same reason, as
    // SPluginNativeEditor::restoreOpenEditors(): a qxa run uses the REAL
    // platform plugin (the suite does not set QT_QPA_PLATFORM=offscreen), so
    // a shown editor would put plugin windows on the developer's desktop in
    // the middle of a suite. The editor is still BUILT and attached, which is
    // what lets a case drive the real gesture and then read the registry
    // (isGenericEditorOpenFor / SPluginNativeEditor::isOpenFor) instead of
    // asserting around the dispatch.
    void openParamEditor( int slotIndex );

    // Static, symmetric with SPluginNativeEditor::isOpenFor()/closeFor()
    // (spluginnativeeditor.h): the generic editor's dialog lives in a
    // MODULE-LEVEL registry keyed by slot (splugineffectstrip.cpp), not on
    // any one strip instance, for the identical reason the native editor's
    // does -- see ensureParamEditor()'s own comment.
    /// COMPACT MODE (proposal 48 D5a, laid out by QBX-201). Below a usable
    /// width a row STACKS -- the plugin name on its own line, one step
    /// smaller than the row's font (`suifonts::denseFont`), and a small
    /// unlabelled bypass checkmark on the line under it -- and the two Add
    /// buttons lose their text.
    ///
    /// NOTHING BECOMES UNREACHABLE, only smaller: Edit is the row's own
    /// double-click, and Reload / Remove are on its context menu. Until
    /// QBX-201 that sentence was false -- there was no context menu, so in
    /// the mixer's mount Remove and Reload were unreachable and a right-click
    /// over a plugin row reached the mixer STRIP's menu instead, offering
    /// "Remove track" as its first item over a gesture aimed at a plugin.
    ///
    /// It exists because the mixer pane mounts this widget in a 60 or 96 px
    /// strip while the full row's layout minimum is far past that -- measured
    /// at 113 px for the two Add buttons alone, which made proposal 48's
    /// AC1.7 fail on a pane that was otherwise clean. A SECOND, compact
    /// insert list in `app/mixerui` was the alternative and was rejected: the
    /// editor registry, the drop handling and the Missing / Unsupported
    /// states would each have to be written again and could then disagree
    /// with this one.
    void setCompact( bool compact );
    bool isCompact() const { return compact_; }

    /// THE ROW'S CONTEXT-MENU COMMANDS (QBX-201): `edit` (open the plugin
    /// window -- the native GUI when the plugin has an embeddable one, the
    /// generic slider list otherwise, exactly as a double-click does),
    /// `bypass` (toggle), `remove` and `reload`. The MENU ITEM calls exactly
    /// this, so a test that drives the command drives the item's own code
    /// path -- which is as close as this repo gets to gating a context menu,
    /// and what `SMixerStrip::runMenuCommand` already settled for.
    ///
    /// Returns false for an unknown command, an absent row, or a command the
    /// row does not offer in its current state: `edit` and `bypass` on a
    /// Missing or Unsupported slot (the placeholder has no parameters and is
    /// already transparent), `reload` on an Active one (there is nothing to
    /// re-resolve). `remove` is offered in every state -- remove-plugin is
    /// Missing-tolerant and a slot one cannot delete is the complaint
    /// CONTRACT inv. 8 was filed under. The menu greys out exactly what this
    /// refuses, so the two are the same decision; `describeRowMenu()` reports
    /// it.
    bool runRowCommand( int slotIndex, const QString &command );

    /// The row's menu AS IT WOULD BE BUILT, without showing anything:
    /// `edit=1,bypass=0,reload=1,remove=1` — one entry per item the menu
    /// offers, in menu order, `1` enabled and `0` greyed, with `*` appended
    /// to a CHECKED item (bypass). An item the menu does not offer at all is
    /// absent from the list, which is why `reload` can be asserted with
    /// `absent=`.
    QString describeRowMenu( int slotIndex ) const;

    /// TRIGGER one item of the row's real menu (QBX-201). Builds the menu
    /// exactly as the right-click does -- the same builder AND the same
    /// signal connections -- finds the item by command name and triggers it,
    /// without ever showing anything. That last part is why this exists: a
    /// QMenu::exec() is modal, a headless modal is a hang, and no qxa case
    /// may open one. What this reaches that `runRowCommand()` alone does not
    /// is the WIRING: the connect() from each QAction to its command.
    ///
    /// Returns false when the row does not exist, when the menu does not
    /// offer that command at all, or when it offers it GREYED -- a user
    /// cannot click a disabled item either, and QAction::trigger() would.
    bool triggerRowMenuItem( int slotIndex, const QString &command );

    /// Deliver a REAL double-click to row `slotIndex`'s container, so the
    /// strip's own event filter runs and dispatches it (QBX-201). The gesture
    /// seam `SMixerStrip::driveControl` is for the same reason: a missing
    /// event filter must FAIL the gate rather than being asserted around.
    /// Returns false when there is no such row.
    bool driveRowDoubleClick( int slotIndex );

    /// What one row's layout demands horizontally, which is the number that
    /// decides whether an insert list can live in a 60/96 px mixer column at
    /// all (proposal 48 AC1.7's own measurement, 113 px, is of this kind).
    /// -1 when there is no such row.
    int rowMinimumWidth( int slotIndex ) const;

    static bool isGenericEditorOpenFor( SPluginSlot *slot );
    static void closeGenericEditorFor( SPluginSlot *slot );

    // --- introspection seams (used by the qxa verbs in app/testkit) ----------
    // These exist because M5 is almost entirely UI: without them the greying of
    // a Missing slot and the editor -> action wiring would have no automated
    // coverage at all.

    int slotRowCount() const { return (int) pluginWidgets_.size(); }

    // A compact, stable description of one row AS THE WIDGET RENDERS IT — not as
    // the model would answer. Shape:
    //   name=<label>|state=Active|mode=Direct|bypass=0|nameEnabled=1|
    //   reload=0|tooltip=<row tooltip>
    //
    // ...with QBX-201's fields APPENDED after it, for the reason `latency=`
    // and `editorOpen=` were appended before them: the M5 cases assert
    // contiguous spans of the older fields and a field inserted among them
    // would break assertions about something else entirely.
    //   compact=0|1              is this the mixer's mount?
    //   nameSmaller=0|1          the name's font is strictly smaller than the
    //                            row's own (the REQUIREMENT, not a point size:
    //                            see suifonts::denseFont for why an absolute
    //                            number could not be asserted off one platform)
    //   nameFontSet=0|1          Qt::WA_SetFont on the name label — equality
    //                            of two fonts passes with the setFont() call
    //                            deleted, on every platform whose themes name
    //                            no dock-title font (QBX-102's own lesson)
    //   bypassBelowName=0|1      the bypass box sits on a LATER line of the
    //                            row's own vertical layout than the name.
    //                            Structural, read from the layout rather than
    //                            from geometry, so it needs no mapped widget
    //                            and cannot read true off a stale layout
    //   bypassText=<label>       empty in compact mode (the checkmark alone)
    //   menu=<describeRowMenu()>
    QString describeSlot( int slotIndex ) const;

    /// "0 frames (0.0 ms)" — the chain's TOTAL reported plugin latency, as the
    /// strip's footer shows it (proposal 21 L5). Reported, never compensated.
    QString chainLatencyText() const;
    /// The same number, unformatted: the sum of every slot's reported latency.
    std::uint32_t chainLatencyFrames() const;

    // Is there an "Add Instrument" affordance right now? It is hidden once the
    // track has an instrument, because a second one is refused.
    bool addInstrumentEnabled() const;

    // Drive the parameter editor for `slotIndex` the way a user drag does, WITHOUT
    // showing a window (a qxa run on Windows uses the real platform plugin, and a
    // test must never pop a dialog onto the user's screen or steal focus).
    // WHICH editor openParamEditor() would open for this slot, without opening
    // anything: "native", "generic", or "none". The fallback decision is the
    // whole safety story of proposal 33 decision D3 — the slider list is not
    // reachable alongside a native GUI, so a plugin that starts reporting a
    // native editor it cannot actually embed would cost the user the editor
    // entirely. Being able to assert the decision is what stops that being
    // discovered by a user.
    QString editorKindFor( int slotIndex );

    bool editorSetParam( int slotIndex, std::uint32_t paramId, double value );

    // ...and DOUBLE-CLICK that parameter's slider, which the production filter
    // turns into "back to the plugin's declared default". Same no-window rule.
    bool editorDoubleClickParam( int slotIndex, std::uint32_t paramId );

    // Headless readback for a qxa case: the text the editor's value label DISPLAYS
    // for parameter `row` of `slotIndex` (after the plugin's own value-to-text).
    // Empty if there is no such editor/row. Pairs with editorSetParam() to prove
    // units / enum names reach the screen, not just the model.
    QString editorValueText( int slotIndex, int row );

    // Headless seam: open (or find) the generic editor for `slotIndex` WITHOUT
    // showing it -- same discipline as editorSetParam()/editorValueText().
    // Exists for the strip-rebuild survival gate (issue a's generic-editor
    // half, 2026-08-23): returns whether an editor now exists.
    // isGenericEditorOpenFor() below reads the same fact back from the
    // durable registry, which is what makes the survival check possible --
    // this call and that query can be made through TWO DIFFERENT strip
    // instances and still agree, because the registry is keyed by slot, not
    // by strip.
    bool ensureGenericEditorForTest( int slotIndex );

protected slots:
    void onAddPluginClicked();
    // The instrument half of Add (proposal 37 6.1): the same browser, opened on
    // the Instruments filter, and the same insert-plugin action - which is what
    // puts the slot at index 0 and refuses a second one (design D3).
    void onAddInstrumentClicked();
    void onRemovePluginClicked( int slotIndex );
    void onPluginBypassToggled( int slotIndex, bool bypassed );
    void onReloadPluginClicked( int slotIndex );
    void onPluginSlotInserted( int index, SPluginSlot &slot );
    void onPluginSlotRemoved( int index, SPluginSlot &slot );
    void onPluginSlotsReordered();

protected:
    void dragEnterEvent( QDragEnterEvent *event ) override;
    void dragMoveEvent( QDragMoveEvent *event ) override;
    void dropEvent( QDropEvent *event ) override;
    // Double-click on a row container opens that row's parameter editor.
    bool eventFilter( QObject *watched, QEvent *event ) override;
    /// The per-row menu (QBX-201). Handled HERE rather than on each row
    /// widget because Qt propagates an unaccepted context-menu event up the
    /// parent chain, translating its position as it goes: one handler on the
    /// strip therefore sees a right-click on the row, on its name, on its
    /// checkbox and on its buttons alike. A click over NO row is left
    /// unaccepted on purpose, so the mount's own menu (the mixer strip's
    /// track commands) still answers for the strip's empty space.
    void contextMenuEvent( QContextMenuEvent *ev ) override;

private:
    struct PluginWidget {
        int          slotIndex;
        SPluginSlot *slot;
        QLabel      *nameLabel;
        QCheckBox   *bypassCheckbox;
        QPushButton *editBtn;
        QPushButton *reloadBtn;   // null when the slot is Active
        QPushButton *removeBtn;
        QWidget     *container;
        QString      tooltip;
        /// The row's OUTER layout: a QVBoxLayout in compact mode (the name on
        /// its own line, the controls under it), the single QHBoxLayout
        /// otherwise. Kept because `bypassBelowName` is read from it.
        QBoxLayout  *rowLayout;
    };

    void rebuildUI();
    /// Which row a point in THIS widget's coordinates is over, or -1.
    int  rowIndexAt( const QPoint &pos ) const;
    /// Fill `menu` with one row's commands. The one builder behind
    /// contextMenuEvent(), describeRowMenu() and triggerRowMenuItem(), so the
    /// menu a user sees and the menu a gate asserts cannot drift apart.
    void buildRowMenu_( int slotIndex, QMenu &menu ) const;
    /// ...and the one place each of its items is wired to its command. Split
    /// from the builder only because describeRowMenu() is const and a
    /// connection needs a non-const receiver; both call sites that show or
    /// trigger a menu call BOTH, so the wiring is never a second spelling.
    void connectRowMenu_( int slotIndex, QMenu &menu );
    // "0" for the first track: the comma-separated index path from the root
    // mixer that every plugin action takes (app/model/sobjectpath.h). Empty when
    // the track is not in the current project's mixer.
    QString trackPathString() const;
    SPluginParamEditor *ensureParamEditor( int slotIndex, bool showWindow );

    STrack       *track_;
    SPluginChain *pluginChain_;
    QVBoxLayout  *pluginsLayout_;
    QPushButton  *addInstrumentBtn_ = nullptr;
    QPushButton  *addEffectBtn_ = nullptr;
    bool          compact_ = false;
    std::vector<PluginWidget> pluginWidgets_;
    int dragSourceIndex_ = -1;

};

#endif
