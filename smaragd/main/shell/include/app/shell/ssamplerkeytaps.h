#ifndef _SSAMPLERKEYTAPS_H_
#define _SSAMPLERKEYTAPS_H_

#include <QObject>
#include <QPointer>
#include <QString>

#include <vector>

#include "tw/devices/midi_in_fanout.h"

class QTimer;
class SApplication;
class SProject;
class SSamplerProgram;

/**
 * SSamplerKeyTaps — every NassauDigital instance's OWN tap of its track's
 * device MIDI input (proposal 56 Q6; the author's answer of 2026-11-02 to
 * "where does the last key played come from").
 *
 * DESIGN §7 lands a recording, in the Key view on a program that has zones,
 * on the LAST KEY PLAYED. That key is what the instance's device MIDI input
 * delivered last — NOT what the engine's event feed played (a sequenced clip
 * plays notes too, and qbx's live event feed has one reader, the engine). So
 * each instance gets a private SINK of its track's MIDI input port
 * (SMidiInputHub's fan-out: its own SPSC ring, filled on the device thread),
 * the tap Q7.2's hand selection will read as well. A main-thread poll drains
 * the rings and hands each note-on (velocity > 0) to
 * SSamplerProgram::setLastKey(). Live state: nothing here is saved.
 *
 * WHICH INSTANCES: every NassauDigital program on the chain of a track whose
 * input is MIDI (`midi:<port>:<ch|any>` or `keyboard`), armed or not, under
 * the master and every arrangement root. The set is recomputed after every
 * action (SProject::arrangementChanged: a program is created by its first
 * edit, a track input is set by a verb) and on liveLanesMayHaveChanged; a tap
 * whose program, port or channel changed is released and acquired anew.
 *
 * THREADING: main thread only, apart from the rings the device threads fill.
 */
class SSamplerKeyTaps : public QObject
{
    Q_OBJECT
public:
    explicit SSamplerKeyTaps( SApplication *app, QObject *parent = nullptr );
    ~SSamplerKeyTaps() override;

    /// The current project changed (SApplication::setCurrentProject).
    void projectChanged( SProject *project );
    /// Recompute the set of taps now.
    void sync();
    /// Drain every tap now (the poll's work; also what a test may call).
    void poll();

    int tapCount() const { return (int) taps_.size(); }

private:
    struct Tap {
        QPointer<SSamplerProgram>  program;
        QString                    port;
        int                        channel = -1;
        audio::MidiInFanout       *fanout  = nullptr;
        audio::MidiInFanout::Sink *sink    = nullptr;
    };
    void releaseAll_();

    SApplication       *app_ = nullptr;
    QPointer<SProject>  project_;
    QTimer             *timer_ = nullptr;
    std::vector<Tap>    taps_;
};

#endif
