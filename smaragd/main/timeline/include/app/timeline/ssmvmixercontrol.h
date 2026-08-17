
#ifndef _SSMVMIXERCONTROL_H
#define _SSMVMIXERCONTROL_H

#include <qwidget.h>
#include <QList>
#include "app/model/sobjectrenderer.h"
#include "tw/metering/tw_level_probe.h"
#include "app/model/sautomationlane.h"

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
    // Right-click on a head shows the arranger's track menu (see
    // SStdMixerView::showTrackContextMenu) — the heads have none of their own.
    void contextMenuEvent( QContextMenuEvent * ) override;
    void resizeEvent( QResizeEvent * ) override;
    // A head has no time axis, so the wheel means what it means over the
    // arranger canvas: the configured scroll / zoom gestures. Child widgets
    // that ignore the wheel (buttons, labels, the meter) propagate here on
    // their own; the FADER keeps its own wheel (1 dB per notch) and is the one
    // deliberate exception.
    void wheelEvent( QWheelEvent * ) override;
    // Watches the fader for a double-click, which resets it to 0.0 dB.
    bool eventFilter( QObject *, QEvent * ) override;

protected slots:
    void sliderValueChanged( int value );
    void sliderValueChanged( double value );

    // Button -> model.
    void muteToggled( bool );
    void soloToggled( bool );
    void armToggled( bool );
    // Name field -> model, through set-track-name so a rename is undoable and
    // actually reaches the project file. Fires on Enter and on focus-out; a
    // no-op when the string did not change, so merely clicking through a head
    // never puts an entry on the undo stack.
    void commitTrackName();
    // Button -> view: expand/collapse this track's take lanes (UI-only state
    // on SStdMixerView; the rebuild this triggers deletes this control via
    // deleteLater, which is safe from inside the handler).
    void takesToggled( bool );
    // "I": open the instrument slot's parameter editor (slot 0). The button is
    // shown only when slot 0 IS an instrument, so this cannot be reached
    // otherwise; the native-editor route is proposal 33 M3.
    void instrumentClicked();
    // "A": automation mode (proposal 37 P6, design D5/6.1). A LEFT click
    // CYCLES Off -> Trim -> Read -> Touch -> Latch -> Write -> Off; a right
    // click picks one from a menu.
    //
    // WHICH LANE IT GOVERNS: **every automation lane the track owns**, its own
    // `self:` lanes and its plugin slots' `param:` lanes alike, in one undo
    // macro. That is the reference-DAW reading of a per-track automation mode
    // (a write pass records what the hand touches, whatever it touched), and
    // it is the only reading under which a single button on the head is not
    // ambiguous the moment a track owns two lanes. A track that owns NO lane
    // yet gets a `self:Volume` lane created in the mode being cycled to, so
    // the button always does something the user can see.
    void automationClicked();
    void showAutomationModeMenu();
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

    // Track selection highlight (the primary moved / the set changed).
    void onSelectedTrackChanged( STrack *track );
    void onSelectionChanged();

private:
    // The tracks this head's M / S / R / G toggles act on: the whole selection
    // when this lane is part of it, this lane alone otherwise. See
    // SStdMixerView::selectionTargets.
    QList<STrack *> toggleTargets() const;

    // Resolve this control's track index within the mixer model (-1 if gone).

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

    // Where the track name sits. The name belongs NEXT TO the M/S/R/T/G
    // buttons; which layout that is depends on how the buttons are arranged and
    // on whether the column is wide enough to hold both:
    //   BesideButtons — Full density: the buttons are a column, so the name goes
    //                   at the top of the right-hand column, beside them.
    //   InButtonRow   — Compact/Tiny: the buttons are a row and the name rides
    //                   at its end, taking the leftover width.
    //   OwnLine       — the fallback when that leftover is too narrow to read
    //                   (a five-button row already fills the 120 px column).
    enum class LabelSpot { BesideButtons, InButtonRow, OwnLine };
    void placeLabel( LabelSpot );
    // Whether a readable name still fits beside `btn`-sized visible buttons.
    bool nameFitsInButtonRow( int btn ) const;
    // Whether a COLUMN of nBtns `btn`-sized buttons still fits this lane's
    // height. It is what decides the strip's shape in Compact density.
    bool buttonColumnFits( int btn, int nBtns ) const;
    // The narrowest name field worth having; below it the name gets its own line.
    static constexpr int NAME_MIN_W = 56;
    LabelSpot labelSpot_ = LabelSpot::BesideButtons;   // where the ctor puts it

public:
    // Test face for the meter (see SMainWindow::describeTrackMeter). Non-const
    // because it re-applies the density rules for the current size first.
    QString describeMeter();

    // Test face for the STRIP (proposal 37 P4, design 6.1). Same shape and
    // same reason as describeMeter: it re-applies the density rules for the
    // current size first, because Qt delivers no resizeEvent to a widget that
    // was never shown. Reads
    //
    //   density=Full|w=120|h=160|btns=M,S,R,T,G,A|I=0|A=1|fitW=1|fitH=1|name=beside
    //
    // `I`/`A` are the instrument and automation buttons' VISIBILITY, and
    // fitW/fitH say the visible strip still fits the lane it was given - the
    // "hiding beats clipping" rule made assertable rather than eyeballed.
    QString describeHead();

    // The track's automation mode: the common mode of every lane it owns, or
    // the mode of the first one when they disagree, or Off when it owns none.
    SAutomationMode trackAutomationMode() const;
    // Set that mode on EVERY lane the track owns, as one undo macro. Creates a
    // `self:Volume` lane when the track owns none.
    void setTrackAutomationMode( SAutomationMode m );
    // TEST ENTRY POINT: push a measured level straight into this head's meter.
    // A head built for a grab is never SHOWN, so onMeterTick's isVisible() gate
    // (the first layer of the repaint-storm defence) would make every tick a
    // no-op and the grab would show a meter sitting at the floor — a picture of
    // the layout with the thing under test invisible in it. Returns the lanes
    // the meter is drawing.
    int tkPushMeterLevel( const twLevelSampleSet &s, qint64 nowMs );

    // TEST ENTRY POINT: press one of this head's toggle buttons ("mute",
    // "solo", "arm", "takes", "group") as a user does, driving the button's
    // own signal — which is what makes the selection BROADCAST the thing under
    // test rather than a re-spelling of it. No-op (true) when the button is
    // already in the requested state; false for an unknown name.
    bool tkClickToggle( const QString &which, bool on );

private slots:
    // Proposal 34: one metering tick. Reads this track's frozen page at the
    // (already latency-compensated) position and feeds the meter, or decays it.
    void onMeterTick( offset_t pos, qint64 nowMs, bool live );
    // Proposal 37 P6: while a Read-family `self:Volume` lane exists, the fader
    // DISPLAYS the curve's value at the position being heard. Driven off the
    // metering tick because that is the one main-thread pump that keeps ticking
    // at a static position and for a tail after the transport stops - a fader
    // frozen at the value the curve had when playback stopped would be a lie.
    void pumpReadValue( offset_t pos );

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

    // Track selection state for styling: in the selection at all, and the
    // PRIMARY of it (the lane the Track Detail dock follows).
    bool selected_ = false;
    bool primary_ = false;

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
    QBoxLayout *qFaderRow_;  // fader column next to (or above) the meter
    QBoxLayout *qRightCol_;  // track name over the fader row
    QBoxLayout *qStripRow_;  // buttons next to (or above) the right column
    QSlider *qVolume_;
    QLabel *qVolLabel_;
    QLineEdit *qTrkLabel_;
    QPushButton *qMute_;
    QPushButton *qSolo_;
    QPushButton *qArm_;
    QPushButton *qTakes_;   // "T": show/hide this track's take lanes
    QPushButton *qGroup_;   // "G": edit-group lock (proposal 17 phase 4)
    QPushButton *qInstr_;   // "I": instrument slot editor (proposal 37 6.1)
    QPushButton *qAuto_;    // "A": automation mode  (proposal 37 6.1)

    // Paint the "A" button for the current mode and refresh its tooltip.
    void refreshAutomationButton();
    // True while pumpReadValue()/an external change writes the slider, so the
    // resulting valueChanged never turns a DISPLAY into a new action.
    bool applyingReadValue_ = false;
    // The dB the read display last wrote, so a static position costs nothing.
    double lastReadDb_ = 1e30;

    // Does this track carry an INSTRUMENT in slot 0? The one question the "I"
    // button's visibility turns on; asked of the model every time the density
    // is applied, because an insert-plugin can change the answer.
    bool hasInstrumentSlot() const;

    // Proposal 34 — level meter beside the fader, and the probe that feeds it.
    // The probe is bound ONCE to the track's root component (its twRewire): the
    // only per-track component that caches pages, and post-fader/post-FX, so the
    // reading is the track's actual contribution to the mix.
    SLevelMeter *qMeter_;
    twLevelProbe probe_;

    // Proposal 36 B8 — tell the meter how many lanes to draw and how many the
    // project has. Returns the drawn count, which is also what the probe is
    // asked for. Called from the ctor, from every tick and from describeMeter(),
    // because a headless caller never gets a tick and would otherwise describe a
    // one-lane meter on a six-channel project.
    int syncMeterLanes();
};

#endif
