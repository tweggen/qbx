#ifndef SMIXERSTRIP_H
#define SMIXERSTRIP_H

#include <functional>

#include <QPointer>
#include <QString>
#include <QWidget>

#include "tw/metering/tw_level_probe.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QHBoxLayout;
class QScrollArea;
class QSlider;
class QVBoxLayout;

class SAction;
class SLevelMeter;
class SPluginEffectStrip;
class SSendStrip;
class SStdMixer;
class STrack;

/**
 * ONE CHANNEL STRIP (proposal 48 M1 / D5).
 *
 * Top to bottom: the name header, the INSERTS (`SPluginEffectStrip`), the
 * SENDS (`SSendStrip`), then M/S/R beside the meter and the fader.
 *
 * **The M/S/R + meter + fader block sits OUTSIDE the strip's scroll area.**
 * `STrackDetailPanel`'s own rule and its own words: those are what a user
 * looks at while the transport runs, so they stay put however far the FX
 * content above them is scrolled.
 *
 * **NOTHING HERE IS A SECOND SPELLING OF AN ARRANGER RULE.** The fader curve
 * is `sfadercurve.h`, the broadcast targets are `strackbroadcast::targetsFor`,
 * audibility is `ssolo::isLaneAudible`, the meter is `SLevelMeter` and the
 * inserts and sends are the arranger's own widgets mounted here. See
 * `main/mixerui/CONTRACT.md` for why that is an invariant rather than a
 * preference.
 *
 * The strip is REBUILT by its owner on a structure change and updated IN
 * PLACE otherwise (CONTRACT inv. 7): a pane that rebuilt on every model
 * signal would delete the fader under the hand mid-drag.
 */
class SMixerStrip : public QWidget
{
    Q_OBJECT

public:
    /// The width of a NARROW strip and of a wide one. AC1.7 gates the layout
    /// at exactly these two.
    static constexpr int NARROW_WIDTH = 60;
    static constexpr int WIDE_WIDTH   = 96;

    /// `rootName` is the ARRANGEMENT this strip belongs to, empty for the
    /// master. It is stamped on every action the strip submits (D12): a pane
    /// showing one arrangement must not commit into whichever tab happens to
    /// be active, which is what `stimeline::submitActive` would do.
    SMixerStrip( SStdMixer *mixer, STrack *track, const QString &rootName,
                 QWidget *parent = nullptr );
    ~SMixerStrip() override;

    STrack *track() const { return track_.data(); }

    /// Narrow keeps M/S/R, the meter and the fader; it hides the name text
    /// beyond an elision, the inserts and the sends (D9). NOT a density
    /// ladder: a strip's width is the USER's choice, unlike a lane's height.
    void setNarrow( bool narrow );
    bool isNarrow() const { return narrow_; }

    /// Which sections this strip shows at all (the pane-wide toggles, M2).
    /// M1 wires the plumbing and always passes every section.
    void setSectionsVisible( bool inserts, bool sends, bool meter, bool fader );

    /// `name=…|narrow=…|mute=…|solo=…|arm=…|db=…|role=…|inserts=…|sends=…|meter=…`
    QString describe() const;

    /// Drive a control the way a HAND would: `mute` / `solo` / `arm` click the
    /// real button so Qt delivers the signal, which is what makes a missing
    /// `connect()` FAIL (proposal 47 M5's lesson, and the reason the sends
    /// strip's own verb exists). `narrow` toggles the strip width.
    bool driveControl( const QString &control, bool on );

    /// Drop every reference into the project: the track, the probe's tap and
    /// the two mounted widgets. Called from the pane's `detachProject()`
    /// (CONTRACT inv. 4 / D13) BEFORE the project is deleted.
    void detachProject();

private slots:
    void onMeterTick( offset_t pos, qint64 nowMs, bool live );
    void onFaderMoved( int value );
    void onFaderReleased();
    void onMuteToggled( bool on );
    void onSoloToggled( bool on );
    void onArmToggled( bool on );
    void onTrackVolumeChanged( double db );
    void onTrackMutedChanged( bool on );
    void onTrackSoloChanged( bool on );
    void onTrackArmedChanged( bool on );

private:
    void submit_( SAction *a ) const;
    void buildUi_();
    void applyVolumeDb_( double db );
    void setFaderSilently_( double db );
    /// The pass-and-macro shape every M/S/R button shares. `apply` builds the
    /// one action for one track; the macro wrapper is D3's deliberate
    /// three-line duplication (the SUBMIT does not extract, only the TARGETS).
    void broadcast_( const char *macroLabel,
                     const std::function<bool( STrack * )> &needsChange,
                     const std::function<void( STrack * )> &submitOne );
    void pumpReadValue_( offset_t pos );
    int  syncMeterLanes_();

    QPointer<SStdMixer> mixer_;
    QString             rootName_;
    QPointer<STrack>    track_;

    QScrollArea *scroll_      = nullptr;   ///< the SCROLLED half
    QWidget     *scrollBody_  = nullptr;
    QLabel      *nameLabel_   = nullptr;
    QPushButton *narrowBtn_   = nullptr;
    SPluginEffectStrip *inserts_ = nullptr;
    SSendStrip         *sends_   = nullptr;

    QWidget     *fixedBlock_  = nullptr;   ///< the PINNED half (D5)
    QHBoxLayout *msrLayout_   = nullptr;
    QHBoxLayout *faderLayout_ = nullptr;
    QPushButton *muteBtn_     = nullptr;
    QPushButton *soloBtn_     = nullptr;
    QPushButton *armBtn_      = nullptr;
    QSlider     *fader_       = nullptr;
    QLabel      *dbLabel_     = nullptr;
    SLevelMeter *meter_       = nullptr;

    twLevelProbe probe_;

    bool narrow_          = false;
    bool showInserts_     = true;
    bool showSends_       = true;
    bool showMeter_       = true;
    bool showFader_       = true;
    bool updating_        = false;   ///< model->view write in progress
    double lastReadDb_    = 1e30;    ///< the Read-family pump's last value
};

#endif // SMIXERSTRIP_H
