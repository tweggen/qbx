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

private:
    struct PluginWidget {
        QLabel *nameLabel;
        QCheckBox *bypassCheckbox;
        QPushButton *removeBtn;
    };

    void rebuildUI();

    STrack *track_;
    SPluginChain *pluginChain_;
    QVBoxLayout *pluginsLayout_;
    std::vector<PluginWidget> pluginWidgets_;
};

#endif
