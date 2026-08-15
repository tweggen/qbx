#include "tw/events/twsmf.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "tw/core/twlog.h"

namespace {

// --------------------------------------------------------------------------
// Byte-level helpers
// --------------------------------------------------------------------------

struct Reader {
    const uint8_t *p = nullptr;
    size_t n = 0;
    size_t i = 0;
    bool   bad = false;

    bool have(size_t k) const { return i + k <= n; }
    uint8_t u8() {
        if (!have(1)) { bad = true; return 0; }
        return p[i++];
    }
    uint16_t u16be() { uint16_t a = u8(); return (uint16_t)((a << 8) | u8()); }
    uint32_t u32be() {
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v = (v << 8) | u8();
        return v;
    }
    // Variable-length quantity: 7 bits per byte, high bit = "more follows".
    uint32_t vlq() {
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            uint8_t b = u8();
            v = (v << 7) | (uint32_t)(b & 0x7F);
            if (!(b & 0x80)) return v;
        }
        bad = true;           // a 5th continuation byte is malformed
        return v;
    }
};

void putU16be(std::vector<uint8_t> &o, uint16_t v) {
    o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)(v & 0xFF));
}
void putU32be(std::vector<uint8_t> &o, uint32_t v) {
    o.push_back((uint8_t)(v >> 24)); o.push_back((uint8_t)(v >> 16));
    o.push_back((uint8_t)(v >> 8));  o.push_back((uint8_t)(v & 0xFF));
}
// MINIMAL VLQ - a non-minimal encoding is legal to read and forbidden to write,
// which is half of what makes an authored file byte-stable.
void putVlq(std::vector<uint8_t> &o, uint32_t v) {
    uint8_t buf[5];
    int n = 0;
    buf[n++] = (uint8_t)(v & 0x7F);
    while ((v >>= 7) != 0) buf[n++] = (uint8_t)((v & 0x7F) | 0x80);
    while (n--) o.push_back(buf[n]);
}

// --------------------------------------------------------------------------
// Meta type <-> kind
// --------------------------------------------------------------------------

twEventKind kindForMeta(uint8_t type) {
    switch (type) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x07: case 0x08:
    case 0x09:                       return twEventKind::Text;
    case 0x05:                       return twEventKind::Lyric;
    case 0x06:                       return twEventKind::Marker;
    case 0x51:                       return twEventKind::Tempo;
    case 0x58:                       return twEventKind::TimeSig;
    case 0x59:                       return twEventKind::KeySig;
    default:                         return twEventKind::Unknown;
    }
}

// --------------------------------------------------------------------------
// Note pairing (read side)
// --------------------------------------------------------------------------

struct RawNoteOn {
    size_t  index;      // into the raw table
    int16_t channel;
    int16_t key;
};

// Pair note-ons with note-offs FIFO and fold each pair into the note-on's
// `duration`. Unmatched note-ons are closed at the track end; unmatched
// note-offs are kept verbatim (a malformed file keeps its information rather
// than silently losing it).
std::vector<twEvent> pairNotes(std::vector<twEvent> raw, int64_t endTick)
{
    std::vector<RawNoteOn> open;
    std::vector<bool> drop(raw.size(), false);

    for (size_t i = 0; i < raw.size(); ++i) {
        twEvent &e = raw[i];
        if (e.kind == twEventKind::NoteOn) {
            open.push_back({ i, e.channel, e.key });
        } else if (e.kind == twEventKind::NoteOff) {
            bool matched = false;
            for (size_t k = 0; k < open.size(); ++k) {
                if (open[k].channel != e.channel || open[k].key != e.key) continue;
                twEvent &on = raw[open[k].index];
                on.duration = e.time - on.time;
                if (on.duration <= 0) on.duration = 1;   // zero-length note
                open.erase(open.begin() + (long)k);
                drop[i] = true;
                matched = true;
                break;
            }
            if (!matched) {
                TW_LOGW("events", "twSmf: note-off with no note-on (ch %d key %d "
                                  "at tick %lld)",
                        (int)e.channel, (int)e.key, (long long)e.time);
            }
        }
    }
    for (const RawNoteOn &o : open) {
        twEvent &on = raw[o.index];
        on.duration = endTick - on.time;
        if (on.duration <= 0) on.duration = 1;
        TW_LOGW("events", "twSmf: note still on at end of track (ch %d key %d); "
                          "closed at tick %lld",
                (int)on.channel, (int)on.key, (long long)endTick);
    }

    std::vector<twEvent> out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) if (!drop[i]) out.push_back(raw[i]);
    return out;
}

// --------------------------------------------------------------------------
// Write ordering
// --------------------------------------------------------------------------

int writeRank(twEventKind k) {
    switch (k) {
    case twEventKind::NoteOff: return 0;   // release before re-attack
    case twEventKind::NoteOn:  return 2;
    default:                   return 1;
    }
}

struct OutEvent {
    int64_t tick = 0;
    int     rank = 1;
    size_t  order = 0;          // input order, for a stable tie-break
    std::vector<uint8_t> bytes; // the encoded event, without its delta
};

bool encodeEvent(const twEventSeq &seq, const twEvent &e, std::vector<uint8_t> &b)
{
    const uint8_t ch = (uint8_t)((e.channel < 0 ? 0 : e.channel) & 0x0F);
    auto clamp7 = [](double v) -> uint8_t {
        int x = (int)(v + (v < 0 ? -0.5 : 0.5));
        if (x < 0) x = 0;
        if (x > 127) x = 127;
        return (uint8_t)x;
    };
    switch (e.kind) {
    case twEventKind::NoteOn:
        b.push_back((uint8_t)(0x90 | ch));
        b.push_back((uint8_t)(e.key & 0x7F));
        b.push_back(clamp7(e.value));
        return true;
    case twEventKind::NoteOff:
        b.push_back((uint8_t)(0x80 | ch));
        b.push_back((uint8_t)(e.key & 0x7F));
        b.push_back(clamp7(e.value));
        return true;
    case twEventKind::PolyPressure:
        b.push_back((uint8_t)(0xA0 | ch));
        b.push_back((uint8_t)(e.key & 0x7F));
        b.push_back(clamp7(e.value));
        return true;
    case twEventKind::ControlChange:
        b.push_back((uint8_t)(0xB0 | ch));
        b.push_back((uint8_t)(e.paramId & 0x7F));
        b.push_back(clamp7(e.value));
        return true;
    case twEventKind::ProgramChange:
        b.push_back((uint8_t)(0xC0 | ch));
        b.push_back(clamp7(e.value));
        return true;
    case twEventKind::ChannelPressure:
        b.push_back((uint8_t)(0xD0 | ch));
        b.push_back(clamp7(e.value));
        return true;
    case twEventKind::PitchBend: {
        int v = (int)(e.value + (e.value < 0 ? -0.5 : 0.5)) + 8192;
        if (v < 0) v = 0;
        if (v > 16383) v = 16383;
        b.push_back((uint8_t)(0xE0 | ch));
        b.push_back((uint8_t)(v & 0x7F));
        b.push_back((uint8_t)((v >> 7) & 0x7F));
        return true;
    }
    case twEventKind::Sysex: {
        const uint8_t status = (e.paramId == 0xF7) ? 0xF7 : 0xF0;
        b.push_back(status);
        putVlq(b, e.payloadSize);
        const uint8_t *pl = seq.payload(e);
        if (pl) b.insert(b.end(), pl, pl + e.payloadSize);
        return true;
    }
    default: break;
    }
    if (twEventIsMetadata(e.kind)) {
        // Every metadata event kept its raw payload on the way in, so it goes
        // back out unaltered - including the ones we never understood.
        b.push_back(0xFF);
        b.push_back((uint8_t)(e.paramId & 0xFF));
        putVlq(b, e.payloadSize);
        const uint8_t *pl = seq.payload(e);
        if (pl) b.insert(b.end(), pl, pl + e.payloadSize);
        return true;
    }
    // Midi1 and the engine-only kinds (ParamValue, Transport, …) are not SMF.
    return false;
}

// --------------------------------------------------------------------------
// One MTrk chunk
// --------------------------------------------------------------------------

bool readTrack(Reader &r, size_t chunkEnd, twSmfTrack &track, std::string *err)
{
    std::vector<twEvent> raw;
    std::vector<uint8_t> arena;

    int64_t tick = 0;
    uint8_t runningStatus = 0;
    int64_t endTick = -1;

    // Payload bytes live in ONE arena the finished sequence owns; an event
    // keeps only the offset/size into it.
    auto stashPayload = [&arena](twEvent &e, const uint8_t *p, size_t n) {
        e.payloadOffset = (uint32_t)arena.size();
        e.payloadSize = (uint32_t)n;
        arena.insert(arena.end(), p, p + n);
    };

    while (r.i < chunkEnd && !r.bad) {
        tick += (int64_t)r.vlq();
        if (r.bad || r.i >= chunkEnd) break;
        uint8_t status = r.p[r.i];
        if (status & 0x80) {
            ++r.i;
            // Running status survives across channel messages only; a system
            // message (0xF0..0xFF) clears it, as the spec requires.
            runningStatus = (status < 0xF0) ? status : 0;
        } else {
            if (runningStatus == 0) {
                if (err) *err = "running status with no preceding status byte";
                return false;
            }
            status = runningStatus;
        }

        twEvent e;
        e.time = tick;

        const uint8_t hi = (uint8_t)(status & 0xF0);
        const int16_t ch = (int16_t)(status & 0x0F);
        if (status == 0xFF) {
            const uint8_t type = r.u8();
            const uint32_t len = r.vlq();
            if (!r.have(len)) { if (err) *err = "truncated meta event"; return false; }
            const uint8_t *pl = r.p + r.i;
            r.i += len;
            if (type == 0x2F) { endTick = tick; break; }   // end of track
            e.kind = kindForMeta(type);
            e.paramId = type;
            e.channel = -1;
            stashPayload(e, pl, len);
            switch (type) {
            case 0x51:
                if (len >= 3) e.value = (double)(((int64_t)pl[0] << 16) |
                                                 ((int64_t)pl[1] << 8) | pl[2]);
                break;
            case 0x58:
                if (len >= 2) { e.value = pl[0]; e.value2 = (double)(1 << pl[1]); }
                break;
            case 0x59:
                if (len >= 2) { e.value = (double)(int8_t)pl[0]; e.value2 = pl[1]; }
                break;
            case 0x03:
                track.name.assign((const char *)pl, len);
                break;
            default: break;
            }
        } else if (status == 0xF0 || status == 0xF7) {
            const uint32_t len = r.vlq();
            if (!r.have(len)) { if (err) *err = "truncated sysex"; return false; }
            e.kind = twEventKind::Sysex;
            e.paramId = status;
            e.channel = -1;
            stashPayload(e, r.p + r.i, len);
            r.i += len;
        } else {
            switch (hi) {
            case 0x80: {
                const uint8_t k = r.u8(), v = r.u8();
                e.kind = twEventKind::NoteOff; e.channel = ch; e.key = k; e.value = v;
                break;
            }
            case 0x90: {
                const uint8_t k = r.u8(), v = r.u8();
                // Velocity 0 IS a note-off - the most common spelling in real
                // files, because it is the one running status makes cheap.
                e.kind = (v == 0) ? twEventKind::NoteOff : twEventKind::NoteOn;
                e.channel = ch; e.key = k; e.value = v;
                break;
            }
            case 0xA0: {
                const uint8_t k = r.u8(), v = r.u8();
                e.kind = twEventKind::PolyPressure;
                e.channel = ch; e.key = k; e.value = v;
                break;
            }
            case 0xB0: {
                const uint8_t c = r.u8(), v = r.u8();
                e.kind = twEventKind::ControlChange;
                e.channel = ch; e.paramId = c; e.value = v;
                break;
            }
            case 0xC0: {
                const uint8_t v = r.u8();
                e.kind = twEventKind::ProgramChange; e.channel = ch; e.value = v;
                break;
            }
            case 0xD0: {
                const uint8_t v = r.u8();
                e.kind = twEventKind::ChannelPressure; e.channel = ch; e.value = v;
                break;
            }
            case 0xE0: {
                const uint8_t lsb = r.u8(), msb = r.u8();
                e.kind = twEventKind::PitchBend; e.channel = ch;
                e.value = (double)(((int)msb << 7) | lsb) - 8192.0;
                break;
            }
            default:
                if (err) *err = "unsupported status byte";
                return false;
            }
        }
        raw.push_back(e);
    }
    if (r.bad) { if (err) *err = "truncated track"; return false; }

    if (endTick < 0) endTick = tick;      // no end-of-track meta: the last event

    // The surviving events keep their offsets into `arena` verbatim - the arena
    // is handed to the sequence unchanged. (Pairing only ever drops note-offs,
    // and a note event never carries a payload.)
    std::vector<twEvent> paired = pairNotes(std::move(raw), endTick);

    track.endTick = endTick;
    track.events = std::make_shared<const twEventSeq>(std::move(paired),
                                                      std::move(arena));
    r.i = chunkEnd;     // skip anything after the end-of-track meta
    return true;
}

}  // namespace

namespace twSmf {

bool read(const uint8_t *data, size_t size, twSmfFile &out, std::string *err)
{
    out = twSmfFile();
    Reader r{ data, size, 0, false };
    if (size < 14 || std::memcmp(data, "MThd", 4) != 0) {
        if (err) *err = "not a Standard MIDI File (no MThd)";
        return false;
    }
    r.i = 4;
    const uint32_t hdrLen = r.u32be();
    const size_t hdrEnd = r.i + hdrLen;
    const uint16_t format = r.u16be();
    const uint16_t ntrks  = r.u16be();
    const int16_t  division = (int16_t)r.u16be();
    if (r.bad || hdrEnd > size) { if (err) *err = "truncated header"; return false; }
    if (division <= 0) {
        if (err) *err = "SMPTE time division is not supported";
        return false;
    }
    if (format > 1) {
        if (err) *err = "SMF type 2 is not supported";
        return false;
    }
    out.format = format;
    out.ppq = division;
    r.i = hdrEnd;                       // a longer header is legal; skip the rest

    for (uint16_t t = 0; t < ntrks; ++t) {
        if (!r.have(8)) { if (err) *err = "truncated track header"; return false; }
        char magic[5] = { 0, 0, 0, 0, 0 };
        std::memcpy(magic, r.p + r.i, 4);
        r.i += 4;
        const uint32_t len = r.u32be();
        if (!r.have(len)) { if (err) *err = "truncated track chunk"; return false; }
        const size_t chunkEnd = r.i + len;
        if (std::memcmp(magic, "MTrk", 4) != 0) {
            // An unknown chunk type is skipped, as the SMF spec requires.
            TW_LOGW("events", "twSmf: skipping unknown chunk '%s'", magic);
            r.i = chunkEnd;
            continue;
        }
        twSmfTrack track;
        if (!readTrack(r, chunkEnd, track, err)) return false;
        out.tracks.push_back(std::move(track));
    }
    if (out.format == 0 && out.tracks.size() > 1) {
        if (err) *err = "type 0 file with more than one track";
        return false;
    }
    return true;
}

bool readFile(const std::string &path, twSmfFile &out, std::string *err)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    return read(buf.data(), buf.size(), out, err);
}

bool write(const twSmfFile &in, std::vector<uint8_t> &out, std::string *err)
{
    out.clear();
    if (in.ppq <= 0) { if (err) *err = "ppq must be positive"; return false; }
    if (in.tracks.empty()) { if (err) *err = "no tracks"; return false; }
    if (in.format == 0 && in.tracks.size() > 1) {
        if (err) *err = "type 0 file with more than one track";
        return false;
    }

    out.insert(out.end(), { 'M', 'T', 'h', 'd' });
    putU32be(out, 6);
    putU16be(out, (uint16_t)in.format);
    putU16be(out, (uint16_t)in.tracks.size());
    putU16be(out, (uint16_t)in.ppq);

    for (const twSmfTrack &tr : in.tracks) {
        std::vector<OutEvent> evs;
        if (tr.events) {
            const twEventSeq &seq = *tr.events;
            evs.reserve(seq.size() * 2);
            for (size_t i = 0; i < seq.size(); ++i) {
                const twEvent &e = seq.events()[i];
                OutEvent oe;
                oe.tick = e.time;
                oe.rank = writeRank(e.kind);
                oe.order = i;
                if (!encodeEvent(seq, e, oe.bytes)) continue;
                evs.push_back(std::move(oe));
                if (e.kind == twEventKind::NoteOn && e.duration > 0) {
                    // The stored note becomes its pair again. Release velocity
                    // is 0: SMF has nowhere to keep one, and inventing a
                    // non-zero value would break the byte-identity gate.
                    OutEvent off;
                    off.tick = e.time + e.duration;
                    off.rank = writeRank(twEventKind::NoteOff);
                    off.order = i;
                    twEvent o;
                    o.kind = twEventKind::NoteOff;
                    o.channel = e.channel; o.key = e.key; o.value = 0.0;
                    encodeEvent(seq, o, off.bytes);
                    evs.push_back(std::move(off));
                }
            }
        }
        std::stable_sort(evs.begin(), evs.end(),
            [](const OutEvent &a, const OutEvent &b) {
                if (a.tick != b.tick) return a.tick < b.tick;
                if (a.rank != b.rank) return a.rank < b.rank;
                return a.order < b.order;
            });

        std::vector<uint8_t> body;
        int64_t last = 0;
        for (const OutEvent &oe : evs) {
            int64_t delta = oe.tick - last;
            if (delta < 0) delta = 0;
            putVlq(body, (uint32_t)delta);
            body.insert(body.end(), oe.bytes.begin(), oe.bytes.end());
            last = oe.tick;
        }
        int64_t endTick = tr.endTick;
        if (endTick < last) endTick = last;
        putVlq(body, (uint32_t)(endTick - last));
        body.insert(body.end(), { 0xFF, 0x2F, 0x00 });

        out.insert(out.end(), { 'M', 'T', 'r', 'k' });
        putU32be(out, (uint32_t)body.size());
        out.insert(out.end(), body.begin(), body.end());
    }
    return true;
}

bool writeFile(const std::string &path, const twSmfFile &in, std::string *err)
{
    std::vector<uint8_t> buf;
    if (!write(in, buf, err)) return false;
    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    f.write((const char *)buf.data(), (std::streamsize)buf.size());
    return f.good();
}

twSmfFile rescalePpq(const twSmfFile &in, int32_t newPpq)
{
    twSmfFile out = in;
    if (newPpq <= 0 || in.ppq <= 0 || newPpq == in.ppq) return out;
    const int64_t oldP = in.ppq, newP = newPpq;
    auto scale = [oldP, newP](int64_t t) -> int64_t {
        // Round half up, in integers: exact - and therefore lossless - whenever
        // oldP divides t * newP, which is the "tick-aligned" case the gate
        // asserts.
        const int64_t num = t * newP;
        return (num >= 0) ? (num + oldP / 2) / oldP : -((-num + oldP / 2) / oldP);
    };
    out.ppq = newPpq;
    for (twSmfTrack &tr : out.tracks) {
        if (!tr.events) continue;
        std::vector<twEvent> evs = tr.events->events();
        for (twEvent &e : evs) {
            const int64_t start = scale(e.time);
            if (e.kind == twEventKind::NoteOn && e.duration > 0) {
                // Scale the END, not the length: scaling both independently
                // drifts a note away from the note that follows it.
                e.duration = scale(e.time + e.duration) - start;
                if (e.duration <= 0) e.duration = 1;
            }
            e.time = start;
        }
        tr.endTick = scale(tr.endTick);
        tr.events = std::make_shared<const twEventSeq>(
            std::move(evs), std::vector<uint8_t>(tr.events->arena()));
    }
    return out;
}

}  // namespace twSmf
