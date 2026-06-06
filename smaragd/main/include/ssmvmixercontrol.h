
#ifndef _SSMVMIXERCONTROL_H
#define _SSMVMIXERCONTROL_H

#include <qwidget.h>
#include <sobjectrenderer.h>

class SStdMixer;
class QGridLayout;
class STrack;
class SStdMixerView;
class SLink;
class QPushButton;
class QSlider;
class QLabel;
class QLineEdit;

class SSMVMixerControl
    : public QWidget
{
    Q_OBJECT
public:
    SSMVMixerControl( 
        QWidget *parent, SStdMixerView &, STrack & );
    virtual ~SSMVMixerControl();
    virtual QSize sizeHint() const;

protected:
protected slots:
    void sliderValueChanged( int value );
    void sliderValueChanged( double value );

    // Button -> model.
    void muteToggled( bool );
    void soloToggled( bool );
    // Model -> button (keeps the buttons in sync if the flag changes elsewhere).
    void onMutedChanged( bool );
    void onSoloChanged( bool );

private:
    // Resolve this control's track index within the mixer model (-1 if gone).
    int trackIndex_() const;

    // Push the slider position to the value v (in dB) without re-submitting
    // an action (model -> view update).
    void setSliderSilently( double v );

    SStdMixerView &smv_;
    STrack &tk_;
    QGridLayout *qLayout_;
    QSlider *qVolume_;
    QLabel *qVolLabel_;
    QLineEdit *qTrkLabel_;
    QPushButton *qMute_;
    QPushButton *qSolo_;
};

#endif
