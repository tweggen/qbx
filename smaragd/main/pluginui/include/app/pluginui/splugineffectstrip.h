#ifndef SPLUGINEFFECTSTRIP_H
#define SPLUGINEFFECTSTRIP_H

#include <QHash>
#include <QPointer>
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
class QVBoxLayout;
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

    // Open (or re-raise) the generic parameter editor for one slot. This is what
    // a double-click on the row and the row's "Edit" button both do.
    void openParamEditor( int slotIndex );

    // --- introspection seams (used by the qxa verbs in app/testkit) ----------
    // These exist because M5 is almost entirely UI: without them the greying of
    // a Missing slot and the editor -> action wiring would have no automated
    // coverage at all.

    int slotRowCount() const { return (int) pluginWidgets_.size(); }

    // A compact, stable description of one row AS THE WIDGET RENDERS IT — not as
    // the model would answer. Shape:
    //   name=<label>|state=Active|mode=Direct|bypass=0|nameEnabled=1|
    //   reload=0|tooltip=<row tooltip>
    QString describeSlot( int slotIndex ) const;

    // Drive the parameter editor for `slotIndex` the way a user drag does, WITHOUT
    // showing a window (a qxa run on Windows uses the real platform plugin, and a
    // test must never pop a dialog onto the user's screen or steal focus).
    bool editorSetParam( int slotIndex, std::uint32_t paramId, double value );

protected slots:
    void onAddPluginClicked();
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
    };

    void rebuildUI();
    // "0" for the first track: the comma-separated index path from the root
    // mixer that every plugin action takes (app/model/sobjectpath.h). Empty when
    // the track is not in the current project's mixer.
    QString trackPathString() const;
    SPluginParamEditor *ensureParamEditor( int slotIndex, bool showWindow );

    STrack       *track_;
    SPluginChain *pluginChain_;
    QVBoxLayout  *pluginsLayout_;
    std::vector<PluginWidget> pluginWidgets_;
    int dragSourceIndex_ = -1;

    // One editor window per slot, keyed by the slot so a rebuild of the strip
    // does not orphan it. QPointer because the dialog is WA_DeleteOnClose.
    QHash<SPluginSlot *, QPointer<QDialog> > editors_;
};

#endif
