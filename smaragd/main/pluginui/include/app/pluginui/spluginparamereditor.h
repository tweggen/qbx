#ifndef SPLUGINPARAMEREDITOR_H
#define SPLUGINPARAMEREDITOR_H

#include <QScrollArea>
#include <memory>
#include <vector>
#include "tw/plugins/twplugin.h"

class QSlider;
class QLabel;

// Generic parameter editor for a plugin.
// Shows a scrollable list of all parameters as sliders with names and values.
class SPluginParamEditor : public QScrollArea {
    Q_OBJECT
public:
    SPluginParamEditor(audio::twPlugin *plugin, QWidget *parent = nullptr);

protected slots:
    void onParamSliderChanged(int sliderIndex);

private:
    struct ParamWidget {
        audio::twPluginParamInfo info;
        QSlider *slider;
        QLabel *valueLabel;
    };

    void buildUI();

    audio::twPlugin *plugin_;
    std::vector<ParamWidget> params_;
};

#endif
