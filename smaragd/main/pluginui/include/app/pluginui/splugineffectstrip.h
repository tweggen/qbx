#ifndef SPLUGINEFFECTSTRIP_H
#define SPLUGINEFFECTSTRIP_H

#include <QWidget>
#include <memory>
#include <vector>

class SPluginSlot;
class SPluginChain;
class STrack;
class QVBoxLayout;
class QCheckBox;
class QPushButton;
class QLabel;
class QDragMoveEvent;
class QDropEvent;

// FX strip widget for a track. Shows ordered list of plugins with:
// - Bypass toggle for each
// - Remove button for each
// - Add button to insert new plugins
// - Drag-to-reorder support via model actions
class SPluginEffectStrip : public QWidget {
    Q_OBJECT
public:
    SPluginEffectStrip(STrack *track, QWidget *parent = nullptr);
    ~SPluginEffectStrip();

protected slots:
    void onAddPluginClicked();
    void onRemovePluginClicked(int slotIndex);
    void onPluginBypassToggled(int slotIndex, bool bypassed);
    void onPluginSlotInserted(int index, SPluginSlot &slot);
    void onPluginSlotRemoved(int index, SPluginSlot &slot);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    struct PluginWidget {
        int slotIndex;
        QLabel *nameLabel;
        QCheckBox *bypassCheckbox;
        QPushButton *removeBtn;
        QWidget *container;
    };

    void rebuildUI();
    void startDragFromPlugin(int pluginIndex);

    STrack *track_;
    SPluginChain *pluginChain_;
    QVBoxLayout *pluginsLayout_;
    std::vector<PluginWidget> pluginWidgets_;
    int dragSourceIndex_ = -1;
};

#endif
