
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

    // The track this control drives (used to re-match controls to tracks after
    // a reorder).
    STrack &getTrack() const { return tk_; }

protected:
    // A grip strip across the top of the control is the drag handle for
    // reordering this track; the rest of the control is its normal channel strip.
    void paintEvent( QPaintEvent * ) override;
    void mousePressEvent( QMouseEvent * ) override;
    void mouseMoveEvent( QMouseEvent * ) override;
    void mouseReleaseEvent( QMouseEvent * ) override;

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

    // Track-reorder drag: armed on press in the grip strip, active once the
    // pointer moves past a small threshold.
    bool dragArmed_;
    bool dragging_;
    QPoint dragPressPos_;

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
