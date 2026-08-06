
#ifndef _SSMVMIXERCONTROL_H
#define _SSMVMIXERCONTROL_H

#include <qwidget.h>
#include "app/model/sobjectrenderer.h"
#include "tw/metering/tw_level_probe.h"

class SLevelMeter;
class SStdMixer;
class QGridLayout;
class QBoxLayout;
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
    virtual QSize sizeHint() const override;

    // The track this control drives (used to re-match controls to tracks after
    // a reorder).
    STrack &getTrack() const { return tk_; }

    // Tree presentation: indent depth, whether this is a foldable parent, and
    // its fold state. The control indents its content, draws a fold triangle for
    // parents, and offsets its grip handle accordingly.
    void setTreeInfo( int depth, bool foldable, bool collapsed );
    int depth() const { return depth_; }

protected:
    // A grip strip across the top of the control is the drag handle for
    // reordering this track; the rest of the control is its normal channel strip.
    void paintEvent( QPaintEvent * ) override;
    void mousePressEvent( QMouseEvent * ) override;
    void mouseMoveEvent( QMouseEvent * ) override;
    void mouseReleaseEvent( QMouseEvent * ) override;
    void resizeEvent( QResizeEvent * ) override;
    // Watches the fader for a double-click, which resets it to 0.0 dB.
    bool eventFilter( QObject *, QEvent * ) override;

protected slots:
    void sliderValueChanged( int value );
    void sliderValueChanged( double value );

    // Button -> model.
    void muteToggled( bool );
    void soloToggled( bool );
    void armToggled( bool );
    // Button -> view: expand/collapse this track's take lanes (UI-only state
    // on SStdMixerView; the rebuild this triggers deletes this control via
    // deleteLater, which is safe from inside the handler).
    void takesToggled( bool );
    // "G": edit-group shortcut — lock this track's subtree together, or
    // dissolve the whole group it belongs to (one undo macro of
    // set-edit-group actions).
    void groupToggled( bool );
    void onEditGroupChanged( int );
    // Model -> button (keeps the buttons in sync if the flag changes elsewhere).
    void onMutedChanged( bool );
    void onSoloChanged( bool );
    void onArmedChanged( bool );

    // Recording channel selection context menu
    void showChannelMenu();
    void setRecordingChannels( uint32_t channels );

    // Track selection highlight
    void onSelectedTrackChanged( STrack *track );

private:
    // Resolve this control's track index within the mixer model (-1 if gone).
    int trackIndex_() const;

    // Push the slider position to the value v (in dB) without re-submitting
    // an action (model -> view update).
    void setSliderSilently( double v );

    // Apply a new track volume (in dB) through the action system. Shared by the
    // fader drag and the double-click-to-reset path.
    void applyVolume_( double dB );

    // Responsive layout management. The strip has to fit whatever lane height
    // it is given — lanes are individually sized and vertical zoom runs down to
    // a few pixels — so the layout adapts to BOTH dimensions. Anything that
    // does not fit is hidden, never clipped.
    void updateLayout();

public:
    // Test face for the meter (see SMainWindow::describeTrackMeter). Non-const
    // because it re-applies the density rules for the current size first.
    QString describeMeter();

private slots:
    // Proposal 34: one metering tick. Reads this track's frozen page at the
    // (already latency-compensated) position and feeds the meter, or decays it.
    void onMeterTick( offset_t pos, qint64 nowMs, bool live );

private:
    static constexpr int WIDE_MODE_THRESHOLD = 156;  // ~130% of minimal width (120px)
    bool wideMode_ = false;
    // Vertical density, by available height:
    //   Full    — name + M/S/R/T/G column + vertical fader + dB readout
    //   Compact — name + one row of small buttons + horizontal fader
    //   Tiny    — name only (plus M/S if they still fit)
    enum class Density { Full, Compact, Tiny };
    static constexpr int DENSITY_FULL_MIN_H    = 132;
    static constexpr int DENSITY_COMPACT_MIN_H = 46;
    Density density_ = Density::Full;
    Density densityFor( int h ) const;
    void applyDensity( Density );

    // Tree presentation state (see setTreeInfo).
    int depth_ = 0;
    bool foldable_ = false;
    bool collapsed_ = false;

    // Track selection state for styling
    bool selected_ = false;

    // Track-reorder drag: armed on press in the grip strip, active once the
    // pointer moves past a small threshold.
    bool dragArmed_;
    bool dragging_;
    QPoint dragPressPos_;

    SStdMixerView &smv_;
    STrack &tk_;
    QGridLayout *qLayout_;
    // Kept as QBoxLayouts (not QV/QHBoxLayout) so the density modes can flip
    // their direction instead of rebuilding the strip.
    QBoxLayout *qBtnCol_;    // M / S / R / T / G
    QBoxLayout *qFaderCol_;  // fader + dB readout
    QBoxLayout *qStripRow_;  // buttons next to (or above) the fader
    QSlider *qVolume_;
    QLabel *qVolLabel_;
    QLineEdit *qTrkLabel_;
    QPushButton *qMute_;
    QPushButton *qSolo_;
    QPushButton *qArm_;
    QPushButton *qTakes_;   // "T": show/hide this track's take lanes
    QPushButton *qGroup_;   // "G": edit-group lock (proposal 17 phase 4)

    // Proposal 34 — level meter beside the fader, and the probe that feeds it.
    // The probe is bound ONCE to the track's root component (its twRewire): the
    // only per-track component that caches pages, and post-fader/post-FX, so the
    // reading is the track's actual contribution to the mix.
    SLevelMeter *qMeter_;
    twLevelProbe probe_;
};

#endif
