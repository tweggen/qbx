#ifndef _S_SEND_TAP_H
#define _S_SEND_TAP_H

#include <QString>

/**
 * ONE SEND TAP: "this object also feeds the send lane called <dest>".
 *
 * Proposal 47 D2. The tap lives on the SOURCE object (an `SObject`, not an
 * `STrack` — the serializer, the verbs and the testkit must reach it without
 * knowing which slice owns it, exactly the argument `contentKind()`,
 * `resolveEventClip()` and the automation-lane vector already make) and it
 * names its destination.
 *
 * THE DESTINATION IS A NAME, NEVER THE `-2-k` SENTINEL. Proposal 45 already
 * restricts `remove-send-lane` to the LAST lane precisely because that
 * sentinel IS the address, so removing one from the middle re-points every
 * path that named a later lane. A stored index would carry the same defect
 * with none of that protection. The name is also already the user-facing
 * address (`$send:Reverb`), and `midiOutPort` is the precedent: a portable
 * NAME in the file, the machine-local id looked up.
 */
struct SSendTap {
    /// The destination send lane's name. Resolved at wiring time; an
    /// unresolved name fails CLOSED and is announced (D6), never silently
    /// dropped.
    QString dest;

    /// Send level in dB, 0 == unity. Applied as the send bus's own per-input
    /// level (D7) — `twMixer::setInputLevel`, no new DSP anywhere.
    double levelDb = 0.0;

    /// D3. false (the default) = POST-fader: the tap is the track's
    /// `twGainStage` output, so the fader scales the send with the dry
    /// signal. true = PRE-fader, i.e. post-FX / pre-fader — the track's
    /// plugin-chain output. A PRE-**FX** tap is deliberately not offered.
    bool preFader = false;

    /// A tap the user has switched off. Kept rather than removed so the level
    /// and the mode survive the round trip, exactly as an automation lane's
    /// mode survives a lane with no points.
    bool enabled = true;

    bool operator==( const SSendTap &o ) const
    {
        return dest == o.dest && levelDb == o.levelDb
            && preFader == o.preFader && enabled == o.enabled;
    }
};

#endif
