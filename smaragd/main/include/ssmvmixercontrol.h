
#ifndef _SSMVMIXERCONTROL_H
#define _SSMVMIXERCONTROL_H

#include <qwidget.h>
#include <sobjectrenderer.h>
#include <qspinbox.h>

class SStdMixer;
class QGridLayout;
class STrack;
class SStdMixerView;
class SLink;
class QPushButton;
class QSpinBox;
class QLineEdit;

class qxDBSpinBox
    : public QSpinBox
{
    Q_OBJECT
public:
    qxDBSpinBox( int min, int max, int step, QWidget *parent );

protected:
    QString mapValueToText( int value );
    int mapTextToValue( bool *ok );
};

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

private:
    SStdMixerView &smv_;
    STrack &tk_;
    QGridLayout *qLayout_;
    qxDBSpinBox *qVolume_;
    QLineEdit *qTrkLabel_;
};

#endif
