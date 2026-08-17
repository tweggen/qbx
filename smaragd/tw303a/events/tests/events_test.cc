// tw/events module test (proposal 37 P0b).
//
// The gate for the event leaf: the SMF corpus round-trips, stateAt agrees with
// a brute-force scan, tick<->frame conversion is EXACT (and stays inside
// Fraction's denominator red line), the automation curve matches its closed
// form, the clip set clamps/loops/slips and synthesises the note-offs a clip
// end owes, and the merge namespaces note ids per source.
//
// Run with `--author-fixtures <dir>` to (re)write the twSmf-AUTHORED half of
// the committed corpus. That mode is not a gate; it is how the byte-identity
// fixtures were produced, kept next to the assertions that consume them so the
// two can never drift.

#include "tw/events/twevent.h"
#include "tw/events/tweventseq.h"
#include "tw/events/tweventsource.h"
#include "tw/events/tweventclipset.h"
#include "tw/events/tweventmerge.h"
#include "tw/events/twtempomap.h"
#include "tw/events/twsmf.h"
#include "tw/events/twautomationcurve.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

#ifndef TW_EVENTS_FIXTURE_DIR
#define TW_EVENTS_FIXTURE_DIR "."
#endif

static std::string fixture(const char *name)
{
    return std::string(TW_EVENTS_FIXTURE_DIR) + "/" + name;
}

static std::vector<uint8_t> readAll(const std::string &path)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

// ===========================================================================
// The authored half of the corpus (--author-fixtures)
// ===========================================================================

static twEvent note(int64_t t, int16_t ch, int16_t key, int vel, int64_t dur)
{
    twEvent e;
    e.time = t; e.kind = twEventKind::NoteOn;
    e.channel = ch; e.key = key; e.value = vel; e.duration = dur;
    return e;
}
static twEvent cc(int64_t t, int16_t ch, uint32_t num, int val)
{
    twEvent e;
    e.time = t; e.kind = twEventKind::ControlChange;
    e.channel = ch; e.paramId = num; e.value = val;
    return e;
}
static twEvent metaEvent(int64_t t, twEventKind k, uint8_t type)
{
    twEvent e;
    e.time = t; e.kind = k; e.paramId = type; e.channel = -1;
    return e;
}

static int authorFixtures(const std::string &dir)
{
    int bad = 0;
    auto put = [&](const char *name, const twSmfFile &f) {
        std::string err;
        if (!twSmf::writeFile(dir + "/" + name, f, &err)) {
            printf("FAIL authoring %s: %s\n", name, err.c_str());
            ++bad;
        } else {
            printf("wrote %s\n", name);
        }
    };

    {   // A plain type 0 file: notes, a controller, a tempo.
        // A metadata event carries its RAW payload - the writer emits exactly
        // those bytes back, so a meta added without one would write an empty
        // meta and lose the tempo.
        twEventSeqBuilder b;
        const uint8_t tempoBytes[3] = { 0x07, 0xA1, 0x20 };   // 500000 = 120 BPM
        twEvent tempo = metaEvent(0, twEventKind::Tempo, 0x51);
        tempo.value = 500000;
        b.addWithPayload(tempo, tempoBytes, 3);
        b.add(note(0, 0, 60, 100, 480));
        b.add(note(480, 0, 64, 90, 480));
        b.add(cc(480, 0, 7, 100));
        b.add(note(960, 0, 67, 80, 960));
        twSmfFile f;
        f.format = 0; f.ppq = 480;
        twSmfTrack t;
        t.events = b.build();
        t.endTick = 1920;
        f.tracks.push_back(t);
        put("authored_simple.mid", f);
    }

    {   // Type 1, three tracks, text + sysex + an unknown meta.
        twSmfFile f;
        f.format = 1; f.ppq = 960;

        twEventSeqBuilder cond;
        const uint8_t tempoBytes[3] = { 0x06, 0x1A, 0x80 };   // 400000 = 150 BPM
        twEvent tempo = metaEvent(0, twEventKind::Tempo, 0x51);
        tempo.value = 400000;
        cond.addWithPayload(tempo, tempoBytes, 3);
        const uint8_t tsBytes[4] = { 0x03, 0x02, 0x18, 0x08 };  // 3/4
        twEvent ts = metaEvent(0, twEventKind::TimeSig, 0x58);
        ts.value = 3; ts.value2 = 4;
        cond.addWithPayload(ts, tsBytes, 4);
        twEvent name = metaEvent(0, twEventKind::Text, 0x03);
        cond.addWithPayload(name, "Conductor", 9);
        twSmfTrack t0; t0.events = cond.build(); t0.endTick = 3840;
        f.tracks.push_back(t0);

        twEventSeqBuilder lead;
        twEvent lname = metaEvent(0, twEventKind::Text, 0x03);
        lead.addWithPayload(lname, "Lead", 4);
        for (int i = 0; i < 8; ++i)
            lead.add(note(i * 480, 1, (int16_t)(60 + i), 90 + i, 240));
        twEvent bend;
        bend.time = 240; bend.kind = twEventKind::PitchBend;
        bend.channel = 1; bend.value = 1024;
        lead.add(bend);
        twSmfTrack t1; t1.events = lead.build(); t1.endTick = 3840;
        f.tracks.push_back(t1);

        twEventSeqBuilder aux;
        const uint8_t sysex[6] = { 0x7E, 0x7F, 0x09, 0x01, 0x00, 0xF7 };
        twEvent sx;
        sx.time = 0; sx.kind = twEventKind::Sysex; sx.paramId = 0xF0; sx.channel = -1;
        aux.addWithPayload(sx, sysex, 6);
        const uint8_t blob[5] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01 };
        twEvent unknown = metaEvent(960, twEventKind::Unknown, 0x7F);
        aux.addWithPayload(unknown, blob, 5);
        twEvent lyric = metaEvent(1920, twEventKind::Lyric, 0x05);
        aux.addWithPayload(lyric, "sing", 4);
        twSmfTrack t2; t2.events = aux.build(); t2.endTick = 3840;
        f.tracks.push_back(t2);

        put("authored_multitrack.mid", f);
    }

    {   // Overlapping same-key notes: the FIFO pairing fixture.
        twEventSeqBuilder b;
        b.add(note(0,   0, 60, 100, 960));    // C4 for two beats
        b.add(note(480, 0, 60, 80, 960));     // the SAME key again, overlapping
        b.add(note(1920, 0, 60, 60, 240));
        twSmfFile f;
        f.format = 0; f.ppq = 480;
        twSmfTrack t; t.events = b.build(); t.endTick = 2400;
        f.tracks.push_back(t);
        put("authored_overlap.mid", f);
    }

    {   // 30 000 events: 10 000 notes and 20 000 controller changes.
        twEventSeqBuilder b;
        b.reserve(30000);
        for (int i = 0; i < 10000; ++i)
            b.add(note(i * 24, (int16_t)(i % 16), (int16_t)(36 + (i % 60)),
                       1 + (i % 126), 12));
        for (int i = 0; i < 20000; ++i)
            b.add(cc(i * 12, (int16_t)(i % 16), (uint32_t)(1 + (i % 100)),
                     i % 128));
        twSmfFile f;
        f.format = 1; f.ppq = 960;
        twSmfTrack t; t.events = b.build(); t.endTick = 10000 * 24;
        f.tracks.push_back(t);
        put("authored_large_30k.mid", f);
    }

    return bad;
}

// ===========================================================================
// (a) the SMF corpus
// ===========================================================================

static bool tablesEqual(const twSmfFile &a, const twSmfFile &b)
{
    if (a.format != b.format || a.ppq != b.ppq) return false;
    if (a.tracks.size() != b.tracks.size()) return false;
    for (size_t i = 0; i < a.tracks.size(); ++i) {
        if (a.tracks[i].endTick != b.tracks[i].endTick) return false;
        if (!a.tracks[i].events || !b.tracks[i].events) return false;
        if (!twEventSeq::equalTables(*a.tracks[i].events, *b.tracks[i].events))
            return false;
    }
    return true;
}

static size_t eventCount(const twSmfFile &f)
{
    size_t n = 0;
    for (const twSmfTrack &t : f.tracks) if (t.events) n += t.events->size();
    return n;
}

static void testSmfCorpus()
{
    struct Case { const char *name; bool authored; };
    const Case corpus[] = {
        { "foreign_type0_running.mid",   false },
        { "foreign_type1_meta_sysex.mid", false },
        { "authored_simple.mid",         true  },
        { "authored_multitrack.mid",     true  },
        { "authored_overlap.mid",        true  },
        { "authored_large_30k.mid",      true  },
    };
    CHECK(sizeof(corpus) / sizeof(corpus[0]) >= 6,
          "the SMF corpus has at least six fixtures");

    for (const Case &c : corpus) {
        const std::string path = fixture(c.name);
        std::vector<uint8_t> bytes = readAll(path);
        char msg[256];

        snprintf(msg, sizeof(msg), "%s: fixture present", c.name);
        CHECK(!bytes.empty(), msg);
        if (bytes.empty()) continue;

        twSmfFile in;
        std::string err;
        snprintf(msg, sizeof(msg), "%s: reads", c.name);
        const bool ok = twSmf::read(bytes.data(), bytes.size(), in, &err);
        CHECK(ok, msg);
        if (!ok) { printf("     (%s)\n", err.c_str()); continue; }

        std::vector<uint8_t> written;
        snprintf(msg, sizeof(msg), "%s: writes", c.name);
        CHECK(twSmf::write(in, written, &err), msg);

        twSmfFile back;
        snprintf(msg, sizeof(msg), "%s: re-reads", c.name);
        CHECK(twSmf::read(written.data(), written.size(), back, &err), msg);

        snprintf(msg, sizeof(msg),
                 "%s: import->export->import is an EQUAL event table (%d events)",
                 c.name, (int)eventCount(in));
        CHECK(tablesEqual(in, back), msg);

        if (c.authored) {
            snprintf(msg, sizeof(msg),
                     "%s: authored by twSmf, so it round-trips BYTE-identically",
                     c.name);
            CHECK(written.size() == bytes.size() &&
                  std::memcmp(written.data(), bytes.data(), bytes.size()) == 0,
                  msg);
        }

        // PPQ rescale is lossless for tick-aligned events: every fixture's
        // ticks are multiples of 12 at 480/960, so 480 -> 960 -> 480 must land
        // exactly back on the original table.
        if (in.ppq == 480 || in.ppq == 960) {
            const int32_t other = (in.ppq == 480) ? 960 : 480;
            twSmfFile up = twSmf::rescalePpq(in, other);
            twSmfFile down = twSmf::rescalePpq(up, in.ppq);
            snprintf(msg, sizeof(msg),
                     "%s: PPQ %d -> %d -> %d is lossless", c.name,
                     in.ppq, other, in.ppq);
            CHECK(tablesEqual(in, down), msg);
        }
    }

    // The reader's two hard cases, asserted on content rather than on shape.
    {
        twSmfFile f;
        std::vector<uint8_t> bytes = readAll(fixture("foreign_type0_running.mid"));
        if (twSmf::read(bytes.data(), bytes.size(), f, nullptr) && !f.tracks.empty()) {
            const twEventSeq &s = *f.tracks[0].events;
            int notes = 0, ccs = 0;
            int64_t firstDur = -1;
            for (const twEvent &e : s.events()) {
                if (e.kind == twEventKind::NoteOn) {
                    if (notes == 0) firstDur = e.duration;
                    ++notes;
                } else if (e.kind == twEventKind::ControlChange) ++ccs;
            }
            CHECK(notes == 3 && ccs == 2,
                  "running status is expanded (3 notes, 2 CCs from one status byte each)");
            CHECK(firstDur == 480,
                  "a note-on with velocity 0 closes the note (C4 lasts 480 ticks)");
        }
    }
    {
        twSmfFile f;
        std::vector<uint8_t> bytes = readAll(fixture("authored_overlap.mid"));
        if (twSmf::read(bytes.data(), bytes.size(), f, nullptr) && !f.tracks.empty()) {
            const twEventSeq &s = *f.tracks[0].events;
            std::vector<int64_t> durs;
            for (const twEvent &e : s.events())
                if (e.kind == twEventKind::NoteOn && e.key == 60)
                    durs.push_back(e.duration);
            CHECK(durs.size() == 3 && durs[0] == 960 && durs[1] == 960,
                  "overlapping same-key notes pair FIFO (both keep 960 ticks)");
        }
    }
    {
        twSmfFile f;
        std::vector<uint8_t> bytes = readAll(fixture("foreign_type1_meta_sysex.mid"));
        if (twSmf::read(bytes.data(), bytes.size(), f, nullptr) && f.tracks.size() == 2) {
            bool tempo = false, sysex = false, unknown = false, lyric = false;
            double tempoUs = 0;
            std::vector<uint8_t> blob;
            for (const twSmfTrack &t : f.tracks) {
                for (const twEvent &e : t.events->events()) {
                    if (e.kind == twEventKind::Tempo) { tempo = true; tempoUs = e.value; }
                    if (e.kind == twEventKind::Lyric) lyric = true;
                    if (e.kind == twEventKind::Sysex) sysex = true;
                    if (e.kind == twEventKind::Unknown && e.paramId == 0x7F) {
                        unknown = true;
                        const uint8_t *p = t.events->payload(e);
                        blob.assign(p, p + e.payloadSize);
                    }
                }
            }
            CHECK(tempo && tempoUs == 500000.0, "a tempo meta decodes to us/quarter");
            CHECK(lyric && sysex, "lyric and sysex survive the import");
            CHECK(unknown && blob.size() == 4 && blob[0] == 0x01 && blob[3] == 0x04,
                  "an unmodelled meta survives as Unknown WITH its payload");
            CHECK(f.tracks[0].name == "Conductor" && f.tracks[1].name == "Lead",
                  "track names are read");
        }
    }
}

// ===========================================================================
// (b) stateAt vs a brute-force scan
// ===========================================================================

// Deliberately naive and independent of twEventSeq::stateAt: no binary search,
// no early exit, one pass over the whole table per query.
static twEventState bruteForceStateAt(const twEventSeq &seq, int64_t p)
{
    twEventState st;
    std::vector<size_t> openIdx;
    for (size_t i = 0; i < seq.events().size(); ++i) {
        const twEvent &e = seq.events()[i];
        if (e.time >= p) continue;
        if (e.kind == twEventKind::NoteOn) {
            if (e.duration > 0) {
                if (e.time + e.duration > p) {
                    twHeldNote h;
                    h.channel = e.channel; h.key = e.key; h.noteId = e.noteId;
                    h.velocity = e.value; h.start = e.time; h.duration = e.duration;
                    h.srcIndex = (int64_t)i;
                    st.notes.push_back(h);
                }
            } else {
                twHeldNote h;
                h.channel = e.channel; h.key = e.key; h.noteId = e.noteId;
                h.velocity = e.value; h.start = e.time; h.duration = 0;
                h.srcIndex = (int64_t)i;
                st.notes.push_back(h);
                openIdx.push_back(st.notes.size() - 1);
            }
        } else if (e.kind == twEventKind::NoteOff) {
            for (size_t k = 0; k < openIdx.size(); ++k) {
                const twHeldNote &h = st.notes[openIdx[k]];
                if (h.channel != e.channel || h.key != e.key) continue;
                const size_t gone = openIdx[k];
                st.notes.erase(st.notes.begin() + (long)gone);
                openIdx.erase(openIdx.begin() + (long)k);
                for (size_t &x : openIdx) if (x > gone) --x;
                break;
            }
        } else if (e.kind == twEventKind::ControlChange) {
            st.cc[{ e.channel, e.paramId }] = e.value;
            if (e.paramId == 64) st.sustain[e.channel] = (e.value >= 64.0);
        } else if (e.kind == twEventKind::PitchBend) {
            st.bend[e.channel] = e.value;
        } else if (e.kind == twEventKind::ChannelPressure) {
            st.pressure[e.channel] = e.value;
        } else if (e.kind == twEventKind::ProgramChange) {
            st.program[e.channel] = (int32_t)e.value;
        }
    }
    st.sortNotes();
    return st;
}

static void testStateAt()
{
    std::mt19937 rng(20260815u);
    std::uniform_int_distribution<int64_t> tdist(0, 100000);
    std::uniform_int_distribution<int> kindDist(0, 9);
    std::uniform_int_distribution<int> chDist(0, 15);
    std::uniform_int_distribution<int> keyDist(24, 96);
    std::uniform_int_distribution<int> valDist(0, 127);
    std::uniform_int_distribution<int64_t> durDist(1, 5000);

    twEventSeqBuilder b;
    for (int i = 0; i < 2000; ++i) {
        twEvent e;
        e.time = tdist(rng);
        e.channel = (int16_t)chDist(rng);
        switch (kindDist(rng)) {
        case 0: case 1: case 2: case 3:
            e.kind = twEventKind::NoteOn; e.key = (int16_t)keyDist(rng);
            e.value = valDist(rng); e.duration = durDist(rng);
            break;
        case 4:   // an OPEN note-on, closed by a later note-off
            e.kind = twEventKind::NoteOn; e.key = (int16_t)keyDist(rng);
            e.value = valDist(rng); e.duration = 0;
            break;
        case 5:
            e.kind = twEventKind::NoteOff; e.key = (int16_t)keyDist(rng);
            break;
        case 6:
            e.kind = twEventKind::ControlChange;
            e.paramId = (uint32_t)valDist(rng); e.value = valDist(rng);
            break;
        case 7:
            e.kind = twEventKind::PitchBend; e.value = valDist(rng) - 64;
            break;
        case 8:
            e.kind = twEventKind::ProgramChange; e.value = valDist(rng);
            break;
        default:
            e.kind = twEventKind::ChannelPressure; e.value = valDist(rng);
            break;
        }
        b.add(e);
    }
    std::shared_ptr<const twEventSeq> seq = b.build();

    int mismatches = 0;
    for (int i = 0; i < 1000; ++i) {
        const int64_t p = tdist(rng);
        if (!(seq->stateAt(p) == bruteForceStateAt(*seq, p))) ++mismatches;
    }
    CHECK(mismatches == 0,
          "stateAt equals a brute-force scan at 1000 random positions of a "
          "2000-event sequence");
    if (mismatches) printf("     (%d mismatches)\n", mismatches);
}

// ===========================================================================
// (c) the tempo map
// ===========================================================================

static void testTempoMap()
{
    twTempoMap map(500000, 960);           // 120 BPM, PPQ 960
    CHECK(std::fabs(map.bpm() - 120.0) < 1e-12, "bpm() is derived from us/quarter");

    // 960 ticks is one quarter; at 120 BPM that is 0.5 s = 24000 frames @ 48k.
    const Fraction f48 = map.ticksToFrames(TickPos((int64_t)960), 48000);
    CHECK(f48 == Fraction(24000), "960 ticks == 24000 frames at 48 kHz, exactly");
    const Fraction f441 = map.ticksToFrames(TickPos((int64_t)960), 44100);
    CHECK(f441 == Fraction(22050), "960 ticks == 22050 frames at 44.1 kHz, exactly");
    const Fraction f96 = map.ticksToFrames(TickPos((int64_t)960), 96000);
    CHECK(f96 == Fraction(48000), "960 ticks == 48000 frames at 96 kHz, exactly");

    // One tick is 25 frames at 48k / 120 BPM, so every multiple of a tick is an
    // integer frame count and framesToTicks must land back on it exactly.
    const int rates[3] = { 44100, 48000, 96000 };
    int roundTripFails = 0;
    int64_t maxDen = 1;
    for (int r = 0; r < 3; ++r) {
        for (int64_t t = 0; t <= 4000; ++t) {
            const Fraction frames = map.ticksToFrames(TickPos(t), rates[r]);
            if (frames.denominator > maxDen) maxDen = frames.denominator;
            const TickPos back = map.framesToTicks(frames.floorToInt(), rates[r]);
            if (frames.isInteger()) {
                if (!(back == TickPos(t))) ++roundTripFails;
            }
        }
    }
    CHECK(roundTripFails == 0,
          "framesToTicks round-trips every tick multiple exactly at 44.1/48/96 kHz");

    // The denominator red line (proposal 18): the reduced denominator of a
    // tick->frame conversion can never exceed ppq * 10^6 (before reduction), a
    // factor 10^7 below Fraction's int64 saturation point.
    const int64_t bound = (int64_t)map.ppq() * 1000000LL;
    CHECK(maxDen <= bound, "conversion denominators stay under ppq * 10^6");
    {
        // A tempo that divides nothing evenly, to show the bound is not an
        // artefact of 500000 us: 130 BPM -> 461538 us/quarter.
        twTempoMap odd;
        odd.setBpm(130.0);
        int64_t worst = 1;
        for (int64_t t = 0; t < 2000; ++t) {
            const Fraction fr = odd.ticksToFrames(TickPos(t), 44100);
            if (fr.denominator > worst) worst = fr.denominator;
        }
        CHECK(worst <= (int64_t)odd.ppq() * 1000000LL,
              "…and at 130 BPM / 44.1 kHz too");
    }

    // Rate independence of the tick domain: the SAME tick is a different frame
    // count at a different rate, and both are exact.
    CHECK(map.quarterNoteFrames(48000) == Fraction(24000) &&
          map.barFrames(48000) == Fraction(96000),
          "quarter and 4/4 bar lengths in frames");
}

// ===========================================================================
// (d) the automation curve
// ===========================================================================

static void testCurve()
{
    std::vector<twCurvePoint> pts;
    pts.push_back({ 0,    0.0, twCurveShape::Linear, 0.0 });
    pts.push_back({ 1000, 1.0, twCurveShape::Step,   0.0 });
    pts.push_back({ 2000, 0.5, twCurveShape::Exp,    2.0 });
    pts.push_back({ 3000, 1.5, twCurveShape::Exp,   -3.0 });
    pts.push_back({ 4000, 0.0, twCurveShape::Linear, 0.0 });
    twAutomationCurve curve(pts);

    // The closed forms, spelled out independently of the implementation.
    auto expected = [&](int64_t f) -> double {
        if (f <= 0) return 0.0;
        if (f >= 4000) return 0.0;
        if (f < 1000) return 0.0 + (1.0 - 0.0) * ((double)f / 1000.0);
        if (f < 2000) return 1.0;                      // Step holds the left value
        if (f < 3000) {
            const double u = (double)(f - 2000) / 1000.0, k = 2.0;
            return 0.5 + (1.5 - 0.5) * ((std::exp(k * u) - 1.0) / (std::exp(k) - 1.0));
        }
        const double u = (double)(f - 3000) / 1000.0, k = -3.0;
        return 1.5 + (0.0 - 1.5) * ((std::exp(k * u) - 1.0) / (std::exp(k) - 1.0));
    };

    double worst = 0.0;
    for (int64_t f = -500; f <= 4500; f += 7)
        worst = std::max(worst, std::fabs(curve.valueAt(f) - expected(f)));
    CHECK(worst <= 1e-12, "valueAt matches the closed forms to 1e-12");
    if (worst > 1e-12) printf("     (worst deviation %.3e)\n", worst);

    // fillRamp is an optimisation, never a second semantics.
    std::vector<double> ramp(2500);
    curve.fillRamp(ramp.data(), 900, (int64_t)ramp.size());
    int rampMismatch = 0;
    for (size_t i = 0; i < ramp.size(); ++i)
        if (std::fabs(ramp[i] - curve.valueAt(900 + (int64_t)i)) > 0.0) ++rampMismatch;
    CHECK(rampMismatch == 0, "fillRamp over [P, P+n) equals n calls of valueAt");

    // Tension 0 IS linear, and an empty curve is its default.
    twAutomationCurve tiny({ { 0, 0.0, twCurveShape::Exp, 0.0 },
                             { 100, 1.0, twCurveShape::Linear, 0.0 } });
    CHECK(std::fabs(tiny.valueAt(50) - 0.5) < 1e-12, "Exp with tension 0 is linear");
    twAutomationCurve none({}, 0.75);
    CHECK(none.valueAt(12345) == 0.75, "an empty curve is its default value");
}

// ===========================================================================
// (e) the clip set
// ===========================================================================

static std::shared_ptr<const twEventSeq> oneNoteSeq(int64_t at, int64_t dur,
                                                    int16_t key = 60)
{
    twEventSeqBuilder b;
    b.add(note(at, 0, key, 100, dur));
    return b.build();
}

static int countKind(const twEventBlock &blk, twEventKind k)
{
    int n = 0;
    for (const twEvent &e : blk.events) if (e.kind == k) ++n;
    return n;
}

static void testClipSet()
{
    // ---- a note held across the clip end -------------------------------
    {
        twEventClipSet set;
        auto seq = oneNoteSeq(0, 10000);
        const int key = 1;
        twFrameRange r = set.insertClip(&key, 1000, 5000,
            [seq](int64_t) { twEventClipResolved rc; rc.seq = seq; return rc; });
        CHECK(r.start == 1000 && r.end == 6000,
              "insertClip returns the clip's timeline extent");

        twEventBlock blk;
        set.collect(0, 8000, blk);
        CHECK(countKind(blk, twEventKind::NoteOn) == 1, "the note-on is collected once");
        CHECK(blk.events.size() == 2, "and it is paired with exactly one note-off");
        const twEvent *off = nullptr;
        for (const twEvent &e : blk.events)
            if (e.kind == twEventKind::NoteOff) off = &e;
        CHECK(off && off->time == 6000,
              "a note held across the clip END gets its note-off AT the clip end");
        CHECK(off && (off->flags & twEventSynthesisedOff),
              "…flagged SynthesisedOff, because the content did not stop there");
        const twEvent *on = nullptr;
        for (const twEvent &e : blk.events)
            if (e.kind == twEventKind::NoteOn) on = &e;
        CHECK(on && off && on->noteId == off->noteId && on->noteId >= 0,
              "the pair carries one note id");

        // ---- a note starting before the window: chase only --------------
        twEventBlock mid;
        set.collect(3000, 1000, mid);
        CHECK(mid.events.empty(),
              "a note that started before the window is NOT re-attacked in it");
        CHECK(mid.chase.notes.size() == 1 &&
              mid.chase.notes[0].key == 60 &&
              mid.chase.notes[0].velocity == 100,
              "it appears in the CHASE set instead, with its velocity");
        CHECK(!mid.chase.notes.empty() && mid.chase.notes[0].start == 1000,
              "the chase reports where it began (clamped to the clip start), "
              "so the pre-roll can reach back to it");
        CHECK(!mid.chase.notes.empty() && on &&
              mid.chase.notes[0].noteId == on->noteId,
              "with the same note id its note-on carried");

        // ---- the window that STARTS on the clip end still owes the off ---
        twEventBlock head, tail;
        set.collect(1000, 5000, head);
        CHECK(countKind(head, twEventKind::NoteOff) == 0,
              "the window ending exactly at the clip end emits no note-off");
        set.collect(6000, 1000, tail);
        CHECK(countKind(tail, twEventKind::NoteOff) == 1 &&
              tail.events[0].time == 0 &&
              (tail.events[0].flags & twEventSynthesisedOff),
              "the NEXT window emits it at offset 0 — the note can never hang");
    }

    // ---- a looped clip repeats at the loop period -----------------------
    {
        twEventSeqBuilder b;
        b.add(note(0,   0, 60, 100, 100));
        b.add(note(500, 0, 64, 100, 100));
        auto seq = b.build();
        twEventClipSet set;
        const int key = 2;
        set.insertClip(&key, 0, 3000, [seq](int64_t) {
            twEventClipResolved rc;
            rc.seq = seq;
            rc.map = twEventLoopMap(0, 1000);   // one-second loop of a 3 s clip
            return rc;
        });
        twEventBlock blk;
        set.collect(0, 3000, blk);
        CHECK(countKind(blk, twEventKind::NoteOn) == 6,
              "a looped clip repeats its events at the loop period (2 x 3)");
        std::vector<int64_t> ons;
        for (const twEvent &e : blk.events)
            if (e.kind == twEventKind::NoteOn) ons.push_back(e.time);
        CHECK(ons.size() == 6 && ons[0] == 0 && ons[1] == 500 && ons[2] == 1000 &&
              ons[3] == 1500 && ons[4] == 2000 && ons[5] == 2500,
              "…at exactly the loop-period positions");
        CHECK(countKind(blk, twEventKind::NoteOff) == 6,
              "and every repeat is released");
    }

    // ---- a note crossing a loop wrap is released AT the wrap -------------
    {
        twEventSeqBuilder b;
        b.add(note(800, 0, 60, 100, 400));   // runs past the 1000-tick loop end
        auto seq = b.build();
        twEventClipSet set;
        const int key = 3;
        set.insertClip(&key, 0, 2000, [seq](int64_t) {
            twEventClipResolved rc;
            rc.seq = seq;
            rc.map = twEventLoopMap(0, 1000);
            return rc;
        });
        twEventBlock blk;
        set.collect(0, 2000, blk);
        std::vector<int64_t> offs, ons;
        for (const twEvent &e : blk.events) {
            if (e.kind == twEventKind::NoteOff) offs.push_back(e.time);
            if (e.kind == twEventKind::NoteOn)  ons.push_back(e.time);
        }
        CHECK(ons.size() == 2 && ons[0] == 800 && ons[1] == 1800,
              "a looped note is re-attacked in the next iteration");
        CHECK(offs.size() == 1 && offs[0] == 1000,
              "a note crossing a loop wrap is released AT the wrap, not left hanging");
        // The second attack runs past the CLIP end, which is this window's end -
        // so, exactly like the clip-end case above, its release belongs to the
        // next window at offset 0.
        twEventBlock next;
        set.collect(2000, 100, next);
        CHECK(next.events.size() == 1 &&
              next.events[0].kind == twEventKind::NoteOff &&
              next.events[0].time == 0 &&
              (next.events[0].flags & twEventSynthesisedOff),
              "…and the one still sounding at the clip end is released at offset 0");
    }

    // ---- slip shifts positions without touching the sequence -------------
    {
        auto seq = oneNoteSeq(500, 100);
        twEventClipSet set;
        const int key = 4;
        set.insertClip(&key, 0, 2000, [seq](int64_t) {
            twEventClipResolved rc;
            rc.seq = seq;
            rc.map = twEventSlipMap(500);    // the clip starts 500 frames in
            return rc;
        });
        twEventBlock blk;
        set.collect(0, 2000, blk);
        CHECK(countKind(blk, twEventKind::NoteOn) == 1 && blk.events[0].time == 0,
              "a slip shifts the collected positions (the note lands at the clip start)");
        CHECK(seq->events()[0].time == 500,
              "…and leaves the sequence itself untouched");
    }

    // ---- mute, update, remove -------------------------------------------
    {
        auto seq = oneNoteSeq(0, 100);
        twEventClipSet set;
        const int key = 5;
        set.insertClip(&key, 0, 1000,
            [seq](int64_t) { twEventClipResolved rc; rc.seq = seq; return rc; });
        twEventBlock blk;
        twFrameRange r = set.setClipMuted(&key, true);
        set.collect(0, 1000, blk);
        CHECK(blk.events.empty() && blk.chase.notes.empty() &&
              r.start == 0 && r.end == 1000,
              "a muted clip contributes nothing, and the mute returns its extent");
        set.setClipMuted(&key, false);
        r = set.updateClip(&key, 500, 1000);
        CHECK(r.start == 0 && r.end == 1500,
              "updateClip returns the UNION of the old and new windows");
        set.collect(0, 2000, blk);
        CHECK(countKind(blk, twEventKind::NoteOn) == 1 && blk.events[0].time == 500,
              "a moved clip moves its events");
        r = set.removeClip(&key);
        set.collect(0, 2000, blk);
        CHECK(blk.events.empty() && r.end == 1500 && set.clipCount() == 0,
              "removeClip returns the vacated extent and the set is empty");
    }

    // ---- metadata never leaves the sequence ------------------------------
    {
        twEventSeqBuilder b;
        b.add(metaEvent(100, twEventKind::Tempo, 0x51));
        b.add(metaEvent(200, twEventKind::Lyric, 0x05));
        b.add(note(300, 0, 60, 100, 100));
        auto seq = b.build();
        twEventClipSet set;
        const int key = 6;
        set.insertClip(&key, 0, 1000,
            [seq](int64_t) { twEventClipResolved rc; rc.seq = seq; return rc; });
        twEventBlock blk;
        set.collect(0, 1000, blk);
        CHECK(countKind(blk, twEventKind::Tempo) == 0 &&
              countKind(blk, twEventKind::Lyric) == 0 &&
              countKind(blk, twEventKind::NoteOn) == 1,
              "metadata kinds stay in the sequence and never reach a consumer");
    }
}

// ===========================================================================
// (f) the merge
// ===========================================================================

static std::shared_ptr<twEventClipSet> setWithNote(const void *key,
                                                   int64_t start, int64_t dur,
                                                   int16_t note_)
{
    auto set = std::make_shared<twEventClipSet>();
    auto seq = oneNoteSeq(0, dur, note_);
    set->insertClip(key, start, dur,
        [seq](int64_t) { twEventClipResolved rc; rc.seq = seq; return rc; });
    return set;
}

static void testMerge()
{
    const int keyA = 1, keyB = 2;
    auto a = setWithNote(&keyA, 0, 1000, 60);
    auto b = setWithNote(&keyB, 0, 1000, 60);   // the SAME key on the SAME channel

    twEventMerge merge;
    merge.setSources({ a, b });

    twEventBlock blk;
    merge.collect(0, 2000, blk);
    CHECK(countKind(blk, twEventKind::NoteOn) == 2,
          "two sources playing the same key yield TWO note-ons");
    CHECK(countKind(blk, twEventKind::NoteOff) == 2, "and two note-offs");

    std::vector<int32_t> onIds, offIds;
    for (const twEvent &e : blk.events) {
        if (e.kind == twEventKind::NoteOn) onIds.push_back(e.noteId);
        if (e.kind == twEventKind::NoteOff) offIds.push_back(e.noteId);
    }
    CHECK(onIds.size() == 2 && onIds[0] != onIds[1],
          "with DISTINCT note ids — the reason ids are namespaced per source");
    CHECK(offIds.size() == 2 && offIds[0] != offIds[1] &&
          ((offIds[0] == onIds[0] && offIds[1] == onIds[1]) ||
           (offIds[0] == onIds[1] && offIds[1] == onIds[0])),
          "and the offs carry the ids their ons did");

    // Sorted output, whatever order the sources came in.
    bool sorted = true;
    for (size_t i = 1; i < blk.events.size(); ++i)
        if (blk.events[i].time < blk.events[i - 1].time) sorted = false;
    CHECK(sorted, "the merged stream is sorted by time");

    // The chase is the union.
    twEventBlock mid;
    merge.collect(500, 100, mid);
    CHECK(mid.chase.notes.size() == 2 &&
          mid.chase.notes[0].noteId != mid.chase.notes[1].noteId,
          "the merged chase set is the UNION of the sources' chases");

    // A controller written by both sources resolves to the last source.
    {
        auto c = std::make_shared<twEventClipSet>();
        twEventSeqBuilder cb;
        cb.add(cc(0, 0, 7, 20));
        auto cseq = cb.build();
        const int keyC = 3;
        c->insertClip(&keyC, 0, 1000,
            [cseq](int64_t) { twEventClipResolved rc; rc.seq = cseq; return rc; });
        auto d = std::make_shared<twEventClipSet>();
        twEventSeqBuilder db;
        db.add(cc(0, 0, 7, 99));
        auto dseq = db.build();
        const int keyD = 4;
        d->insertClip(&keyD, 0, 1000,
            [dseq](int64_t) { twEventClipResolved rc; rc.seq = dseq; return rc; });
        twEventMerge m2;
        m2.setSources({ c, d });
        twEventBlock cblk;
        m2.collect(500, 100, cblk);
        auto it = cblk.chase.cc.find({ 0, 7 });
        CHECK(it != cblk.chase.cc.end() && it->second == 99,
              "a controller both sources wrote resolves to the LAST source, "
              "deterministically");
    }

    // A source removed mid-stream is simply gone from the next collect.
    CHECK(merge.removeSource(b.get()) && merge.sourceCount() == 1,
          "removeSource drops the source");
    twEventBlock after;
    merge.collect(0, 2000, after);
    CHECK(countKind(after, twEventKind::NoteOn) == 1 &&
          countKind(after, twEventKind::NoteOff) == 1,
          "a source removed mid-stream is dropped cleanly");
    b.reset();                       // and its lifetime ended with the merge's ref
    merge.collect(0, 2000, after);
    CHECK(countKind(after, twEventKind::NoteOn) == 1,
          "…and destroying it afterwards changes nothing");
}

// ===========================================================================

int main(int argc, char **argv)
{
    if (argc >= 3 && std::string(argv[1]) == "--author-fixtures")
        return authorFixtures(argv[2]);

    printf("-- (a) SMF corpus\n");
    testSmfCorpus();
    printf("-- (b) stateAt\n");
    testStateAt();
    printf("-- (c) tempo map\n");
    testTempoMap();
    printf("-- (d) automation curve\n");
    testCurve();
    printf("-- (e) event clip set\n");
    testClipSet();
    printf("-- (f) event merge\n");
    testMerge();

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall events tests passed\n", failures);
    return failures ? 1 : 0;
}
