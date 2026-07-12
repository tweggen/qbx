#include "app/pluginui/spluginparamereditor.h"
#include "tw/plugins/twplugin.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QWidget>

SPluginParamEditor::SPluginParamEditor(audio::twPlugin *plugin, QWidget *parent)
    : QScrollArea(parent), plugin_(plugin)
{
    setWidgetResizable(true);
    buildUI();
}

void SPluginParamEditor::buildUI()
{
    if (!plugin_) {
        return;
    }

    QWidget *container = new QWidget();
    QVBoxLayout *mainLayout = new QVBoxLayout(container);

    const size_t paramCount = plugin_->paramCount();
    params_.reserve(paramCount);

    for (size_t i = 0; i < paramCount; ++i) {
        const auto info = plugin_->paramInfo(i);

        // Param row: label + slider + value label
        QHBoxLayout *paramLayout = new QHBoxLayout();

        // Parameter name label (30% width)
        QLabel *nameLabel = new QLabel(QString::fromStdString(info.name));
        nameLabel->setMinimumWidth(120);
        paramLayout->addWidget(nameLabel);

        // Slider (60% width)
        QSlider *slider = new QSlider(Qt::Horizontal);
        slider->setMinimum(0);
        slider->setMaximum(1000);  // 0.1% precision
        double normalized = (plugin_->getParam(info.id) - info.minValue) /
                           (info.maxValue - info.minValue);
        slider->setValue((int)(normalized * 1000));
        paramLayout->addWidget(slider);

        // Value label (10% width)
        QLabel *valueLabel = new QLabel();
        valueLabel->setMinimumWidth(60);
        valueLabel->setAlignment(Qt::AlignRight);
        paramLayout->addWidget(valueLabel);

        // Store in our list
        ParamWidget pw;
        pw.info = info;
        pw.slider = slider;
        pw.valueLabel = valueLabel;
        params_.push_back(pw);

        // Connect slider to value update
        int paramIndex = (int)i;
        connect(slider, &QSlider::valueChanged, this, [this, paramIndex]() {
            onParamSliderChanged(paramIndex);
        });

        // Update display label
        onParamSliderChanged(paramIndex);

        mainLayout->addLayout(paramLayout);
    }

    mainLayout->addStretch();
    setWidget(container);
}

void SPluginParamEditor::onParamSliderChanged(int sliderIndex)
{
    if (sliderIndex < 0 || sliderIndex >= (int)params_.size() || !plugin_) {
        return;
    }

    ParamWidget &pw = params_[sliderIndex];
    const auto &info = pw.info;

    // Convert slider value (0-1000) to parameter value (min-max)
    double normalized = pw.slider->value() / 1000.0;
    double value = info.minValue + normalized * (info.maxValue - info.minValue);

    // Set the parameter
    plugin_->setParam(info.id, value);

    // Update display label with current value
    QString displayText;
    if (info.isStepped) {
        displayText = QString::number((int)value);
    } else {
        displayText = QString::number(value, 'f', 2);
    }
    pw.valueLabel->setText(displayText);
}
