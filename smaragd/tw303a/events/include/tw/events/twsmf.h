#ifndef _TW_SMF_H_
#define _TW_SMF_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tw/events/tweventseq.h"

/**
 * twSmf - Standard MIDI File (type 0 and type 1) reader and writer.
 *
 * The tables it produces are in MUSICAL TICKS at the file's own division
 * (twSmfFile::ppq) - never frames. Converting them is twTempoMap's job and
 * nobody else's (D2).
 *
 * What the reader guarantees:
 *   - RUNNING STATUS is expanded (the single most common reason a hand-written
 *     parser reads garbage from real-world files);
 *   - note-on/note-off pairs become ONE NoteOn carrying `duration`, with
 *     overlapping same-key notes paired FIFO - a note-on of a key already
 *     sounding opens a second note, and the first note-off closes the OLDER
 *     one, which is how every DAW and the MIDI spec resolve it. A note-on with
 *     velocity 0 IS a note-off;
 *   - every meta event keeps its RAW payload bytes, whether or not we
 *     understand it: the ones we do also decode into value/value2 (tempo -> µs
 *     per quarter, time signature -> num/den, key signature -> sf/mi), the ones
 *     we do not become twEventKind::Unknown with `paramId` = the meta type. So
 *     a file written by another program keeps everything it carried, and an
 *     export can put it back byte for byte.
 *   - `paramId` on ANY metadata event is the SMF meta type byte; on a
 *     ControlChange it is the controller number; on a Sysex it is the leading
 *     status byte (0xF0 or 0xF7), so the two sysex spellings survive.
 *
 * What the writer guarantees: one canonical spelling per file - explicit status
 * bytes (no running status), minimal VLQ deltas, note-offs before note-ons at
 * the same tick, and end-of-track at the recorded end. Consequence, and the
 * gate: a file the writer AUTHORED round-trips byte-identically through
 * read + write. A foreign file round-trips to an EQUAL EVENT TABLE, not to
 * equal bytes - it may have used running status, a different event order at
 * one tick, or a non-minimal delta, and normalising those is the point.
 *
 * Not supported: SMPTE division (a negative division word) is rejected with an
 * error rather than silently misread as PPQ, and type 2 files are rejected
 * (independent sequences, no common timebase - nothing in this app models one).
 */

struct twSmfTrack {
    std::string                       name;     // the 0x03 meta, if the file has one
    std::shared_ptr<const twEventSeq> events;   // times in TICKS
    int64_t                           endTick = 0;  // the end-of-track meta's tick
};

struct twSmfFile {
    int32_t format = 1;          // 0 = one track, 1 = parallel tracks
    int32_t ppq = 960;           // ticks per quarter note
    std::vector<twSmfTrack> tracks;
};

namespace twSmf {

// Parse a whole file image. Returns false and fills `err` (when non-null) on a
// malformed or unsupported file; `out` is then unspecified.
bool read(const uint8_t *data, size_t size, twSmfFile &out, std::string *err = nullptr);
bool readFile(const std::string &path, twSmfFile &out, std::string *err = nullptr);

// Serialize. Returns false on a structurally impossible request (format 0 with
// more than one track, no tracks, a non-positive ppq).
bool write(const twSmfFile &in, std::vector<uint8_t> &out, std::string *err = nullptr);
bool writeFile(const std::string &path, const twSmfFile &in, std::string *err = nullptr);

// Re-express every tick at a new division. Exact - and therefore LOSSLESS -
// for events whose ticks are multiples of oldPpq/gcd(oldPpq, newPpq); anything
// finer rounds to the nearest new tick (round-half-up), which is the only
// choice that does not accumulate a bias over a long file.
twSmfFile rescalePpq(const twSmfFile &in, int32_t newPpq);

}  // namespace twSmf

#endif // _TW_SMF_H_
