#ifndef SSENDSTRIP_H
#define SSENDSTRIP_H

#include <QWidget>
#include <QList>
#include <QString>

class STrack;
class QCheckBox;
class QDoubleSpinBox;
class QComboBox;
class QVBoxLayout;

/**
 * The SENDS section of the Track Detail dock (proposal 47 M5).
 *
 * One row per send lane in the arrangement: an enable box, the lane's name, a
 * level in dB and a pre/post choice. It is the first surface that makes M0's
 * verbs reachable by hand — until now `add-send` / `set-send` existed only in
 * a `.qxa` script, exactly as proposal 41's fragment verbs did before its
 * menu items landed.
 *
 * **A ROW EXISTS PER SEND LANE, NOT PER TAP**, and that is the decision this
 * class rests on. A tap is created by TICKING a row, so a user who wants to
 * send somewhere does not first have to discover a separate "add" gesture;
 * and a lane with no tap still shows, so "there is a Reverb bus and this track
 * does not feed it" is visible rather than inferred from an absence.
 *
 * **UNTICKING DISABLES; IT DOES NOT REMOVE.** `SSendTap::enabled` exists so a
 * level and a pre/post choice survive being switched off — the same argument
 * an automation lane's mode surviving an empty lane makes. Removing the tap
 * would silently discard both, and a user toggling a send off and on again
 * would find it back at 0 dB post.
 *
 * Every control commits through the ORDINARY verbs, so every gesture is one
 * undo step and the UI cannot reach a state a script could not (the
 * cycle refusal included — a send lane's own strip can refuse a row).
 */
class SSendStrip : public QWidget {
    Q_OBJECT
public:
    explicit SSendStrip( STrack *track, QWidget *parent = nullptr );

    /// `lane=<name>|on=<0|1>|level=<dB>|pre=<0|1>|enabled=<0|1>` per row,
    /// rows separated by ';'. Read by `assert-send-strip`; the same string the
    /// controls are built from, so a gate cannot pass over a row that is not
    /// really there.
    QString describe() const;

    /// Drive one row's control the way a hand would, for `send-strip-set`.
    /// `control` is "on" | "level" | "pre". Returns false when there is no
    /// row for `laneName`.
    bool driveControl( const QString &laneName, const QString &control,
                       double value );

private slots:
    void onToggled( int row, bool on );
    void onLevelChanged( int row, double db );
    void onModeChanged( int row, int index );

private:
    struct Row {
        QString         lane;
        QCheckBox      *on = nullptr;
        QDoubleSpinBox *level = nullptr;
        QComboBox      *mode = nullptr;
    };

    void rebuild();
    /// The track's path from the mixer root, which every verb wants as its
    /// `track=`. Empty when the track is not reachable, in which case nothing
    /// is committed rather than something being committed to the wrong lane.
    QString trackSpec() const;

    STrack      *track_ = nullptr;
    QVBoxLayout *layout_ = nullptr;
    QList<Row>   rows_;
    /// Set while a commit is repopulating the controls, so a programmatic
    /// setValue() does not re-enter as if the user had moved it. The
    /// automation recorder's `applyVolume_` guard, one class over.
    bool         updating_ = false;
};

#endif
