// tw/record CAPTURE BRIDGE gate (proposal 21 L3a).
//
// Six things, none of which had a gate before (the module's CONTRACT said
// "manual (Ctrl-R with an armed track) ... no headless coverage yet"):
//
//   (a) twGrowingCaptureSource as a data structure: chunked planar storage
//       written across chunk boundaries in odd-sized appends, read back by
//       position per channel and interleaved-with-a-mask, a read past the
//       frontier that is a SHORT READ rather than a wait, the index-exhausted
//       refusal, and the one-copy handover to a fixed-size twCapturingSource.
//
//   (b) SAMPLE-EXACTNESS end to end, at 2 and at 6 channels. A paced
//       FileAudioInput (L0) replays a fixture this test generated, and both
//       the pages AND the WAV the bridge wrote must equal the file frame for
//       frame, sample for sample. Not an RMS band: the capture path has no
//       arithmetic in it at all when the device rate is the project rate, so
//       anything less than equality would be hiding a defect.
//
//   (c) THE BACKPRESSURE CLAIM (design D7). A WAV writer that is far too slow
//       must not stall the ring: `ringOverruns == 0` while `wavLate > 0`, and
//       the file must STILL be complete and byte-identical, because it is
//       finalised out of the pages rather than out of the ring. A test writer
//       that sleeps per write is the only way to assert this — a real writer
//       on this box is never slow enough.
//
//   (d) The LIVE-LANE sink: the third fan-out, pulled in the pump's own shape
//       (planar buffers, short reads allowed) — once from a thread running
//       concurrently with the capture, once after the fact.
//
//   (e) The per-sink CHANNEL MASK: one capture, two files of different widths,
//       neither of which costs a second copy of the audio.
//
//   (f) RecordingSession as a BRIDGE CONSUMER: the public shape the app uses
//       (start / requestStop / isFinished / createdFiles) over the env-selected
//       file backend.
//
// Like devices_input_test this drives a REAL-TIME PACED device, so it measures
// the machine as much as the code and is RUN_SERIAL. It is threaded — loop it
// (>= 20 iterations) when touching the bridge.

#include "tw/record/capture_bridge.h"
#include "tw/record/recording_session.h"
#include "tw/sources/twgrowingcapturesource.h"
#include "tw/sources/twcapturingsource.h"
#include "tw/sinks/audio_file_writer.h"
#include "tw/devices/audio_input.h"

#include "file_input.h"          // PRIVATE to tw_devices (src/), see CMakeLists

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { std::printf("ok   %s\n", msg); }                        \
        else      { std::printf("FAIL %s\n", msg); ++failures; }            \
    } while (0)

namespace {

// ---------------------------------------------------------------------------
// A 16-bit PCM WAV fixture with per-channel-distinct content.
//
// Generated, not committed: both sides of every comparison below come from
// this one function, so there is no third artifact to keep in step. 16-bit is
// deliberate — WAVWriter writes PCM_16, so a 16-bit fixture round-trips
// EXACTLY through float and the equality claim is a claim about the bridge
// rather than about a quantiser.
// ---------------------------------------------------------------------------

std::int16_t fixtureSample(std::size_t frame, std::uint32_t ch)
{
    // Deterministic, per-channel-distinct, and nowhere near full scale so no
    // clipping rule of any writer can be involved.
    std::uint32_t h = (std::uint32_t) frame * 2654435761u + (ch + 1u) * 40503u;
    h ^= h >> 13;
    return (std::int16_t)( (int)( h % 60001u ) - 30000 );
}

void wrU32(unsigned char *p, std::uint32_t v)
{
    p[0] = (unsigned char)( v & 0xFF );        p[1] = (unsigned char)( (v >> 8) & 0xFF );
    p[2] = (unsigned char)( (v >> 16) & 0xFF ); p[3] = (unsigned char)( (v >> 24) & 0xFF );
}
void wrU16(unsigned char *p, std::uint16_t v)
{
    p[0] = (unsigned char)( v & 0xFF ); p[1] = (unsigned char)( (v >> 8) & 0xFF );
}
std::uint32_t rdU32(const unsigned char *p)
{
    return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) |
           ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
}
std::uint16_t rdU16(const unsigned char *p)
{
    return (std::uint16_t)( (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) );
}

bool writeFixtureWav(const std::string &path, int rate,
                     std::uint32_t channels, std::size_t frames)
{
    std::vector<std::int16_t> pcm( frames * channels );
    for( std::size_t f = 0; f < frames; ++f )
        for( std::uint32_t c = 0; c < channels; ++c )
            pcm[ f * channels + c ] = fixtureSample( f, c );

    const std::uint32_t dataBytes = (std::uint32_t)( pcm.size() * 2 );
    unsigned char h[44];
    std::memcpy( h, "RIFF", 4 );
    wrU32( h + 4, 36 + dataBytes );
    std::memcpy( h + 8, "WAVEfmt ", 8 );
    wrU32( h + 16, 16 );
    wrU16( h + 20, 1 );
    wrU16( h + 22, (std::uint16_t) channels );
    wrU32( h + 24, (std::uint32_t) rate );
    wrU32( h + 28, (std::uint32_t)( rate * channels * 2 ) );
    wrU16( h + 32, (std::uint16_t)( channels * 2 ) );
    wrU16( h + 34, 16 );
    std::memcpy( h + 36, "data", 4 );
    wrU32( h + 40, dataBytes );

    std::FILE *f = std::fopen( path.c_str(), "wb" );
    if( !f ) return false;
    std::fwrite( h, 1, 44, f );
    std::fwrite( pcm.data(), 2, pcm.size(), f );
    std::fclose( f );
    return true;
}

// Minimal PCM16 WAV reader — enough to read back what WAVWriter produced.
bool readPcm16Wav(const std::string &path, std::vector<std::int16_t> &out,
                  std::uint32_t &channels, std::uint32_t &rate)
{
    std::FILE *f = std::fopen( path.c_str(), "rb" );
    if( !f ) return false;
    std::fseek( f, 0, SEEK_END );
    const long size = std::ftell( f );
    std::fseek( f, 0, SEEK_SET );
    if( size < 44 ) { std::fclose( f ); return false; }
    std::vector<unsigned char> buf( (std::size_t) size );
    const std::size_t got = std::fread( buf.data(), 1, buf.size(), f );
    std::fclose( f );
    if( got != buf.size() ) return false;
    if( std::memcmp( buf.data(), "RIFF", 4 ) || std::memcmp( buf.data() + 8, "WAVE", 4 ) )
        return false;

    std::uint16_t bits = 0, fmt = 0;
    const unsigned char *data = nullptr;
    std::size_t dataBytes = 0;
    std::size_t p = 12;
    while( p + 8 <= buf.size() ) {
        const char *id = (const char *) &buf[ p ];
        const std::uint32_t sz = rdU32( &buf[ p + 4 ] );
        const std::size_t body = p + 8;
        if( body + sz > buf.size() ) break;
        if( !std::memcmp( id, "fmt ", 4 ) && sz >= 16 ) {
            fmt      = rdU16( &buf[ body ] );
            channels = rdU16( &buf[ body + 2 ] );
            rate     = rdU32( &buf[ body + 4 ] );
            bits     = rdU16( &buf[ body + 14 ] );
        } else if( !std::memcmp( id, "data", 4 ) ) {
            data = &buf[ body ];
            dataBytes = sz;
        }
        p = body + sz + ( sz & 1 );
    }
    if( !data || fmt != 1 || bits != 16 ) return false;
    out.resize( dataBytes / 2 );
    for( std::size_t i = 0; i < out.size(); ++i )
        out[ i ] = (std::int16_t) rdU16( data + i * 2 );
    return true;
}

// ---------------------------------------------------------------------------
// A WAV writer that is FAR too slow, wrapped around the real one.
// ---------------------------------------------------------------------------
class SlowWavWriter : public audio::AudioFileWriter {
public:
    explicit SlowWavWriter( int sleepMs )
        : inner_( audio::createAudioFileWriter( audio::AudioFormat::WAV ) ),
          sleepMs_( sleepMs ) {}

    bool open( const std::string &path, const audio::AudioFileConfig &cfg ) override
    { return inner_ && inner_->open( path, cfg ); }

    bool write( const float *interleaved, std::size_t frameCount ) override
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( sleepMs_ ) );
        return inner_ && inner_->write( interleaved, frameCount );
    }

    bool close() override { return inner_ ? inner_->close() : true; }
    const char *errorMessage() const override
    { return inner_ ? inner_->errorMessage() : ""; }

private:
    std::unique_ptr<audio::AudioFileWriter> inner_;
    int sleepMs_;
};

std::string gTmpDir;

std::string tmpPath( const char *name )
{
    return gTmpDir + "/" + name;
}

// ---------------------------------------------------------------------------
// One paced capture run through the bridge.
// ---------------------------------------------------------------------------
struct RunResult {
    audio::CaptureBridgeStats stats;
    std::vector<float>        live;      // interleaved, what pullLive delivered
    bool                      started = false;
};

RunResult runCapture( const std::string &fixture, std::size_t frames,
                      std::uint32_t channels,
                      const std::vector<audio::CaptureWavSink> &sinks,
                      int slowMs,                 // 0 == the real writer
                      bool concurrentLivePuller,
                      std::shared_ptr<twGrowingCaptureSource> &sourceOut )
{
    RunResult r;

    audio::FileAudioInput input( fixture );
    input.setLoop( false );
    if( input.openDevice() < 0 ) return r;

    audio::CaptureBridgeParams bp;
    bp.externalInput  = &input;
    bp.targetRate     = 48000;
    bp.wavSinks       = sinks;
    // Big enough that nothing the live lane does can overflow it during this
    // run: the fan-out is what is under test here, not the ring's overrun rule
    // (audio_ring.h has its own gate for that).
    bp.liveRingFrames = 1u << 17;
    // Small chunks on purpose — a 65536-frame default would put the whole
    // fixture in one chunk and never exercise a boundary.
    bp.chunkFrames    = 8192;
    if( slowMs > 0 ) {
        bp.writerFactory = [slowMs]() {
            return std::unique_ptr<audio::AudioFileWriter>( new SlowWavWriter( slowMs ) );
        };
    }

    audio::CaptureBridge bridge;
    if( !bridge.start( bp ) ) {
        std::printf( "     bridge start failed: %s\n", bridge.errorMessage() );
        input.closeDevice();
        return r;
    }
    r.started = true;

    std::atomic<bool> pulling{ concurrentLivePuller };
    std::thread puller;
    std::vector<float> live;
    if( concurrentLivePuller ) {
        puller = std::thread( [&] {
            std::vector<std::vector<float> > planar( channels,
                                                     std::vector<float>( 1024, 0.0f ) );
            std::vector<float *> ptrs( channels );
            for( std::uint32_t c = 0; c < channels; ++c ) ptrs[ c ] = planar[ c ].data();
            for( ;; ) {
                const std::size_t got = bridge.pullLive( ptrs.data(), channels, 1024 );
                if( got ) {
                    const std::size_t base = live.size();
                    live.resize( base + got * channels );
                    for( std::size_t i = 0; i < got; ++i )
                        for( std::uint32_t c = 0; c < channels; ++c )
                            live[ base + i * channels + c ] = planar[ c ][ i ];
                } else {
                    if( !pulling.load() ) break;
                    std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
                }
            }
        } );
    }

    // Wait for the paced device to play the file out, then stop. The bound is
    // generous (8x real time) because this is a wall-clock test on a shared box.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              (long long)( 8000.0 * frames / 48000.0 ) + 2000 );
    while( !input.atEnd() && std::chrono::steady_clock::now() < deadline )
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );

    sourceOut = bridge.source();
    bridge.stop();

    if( concurrentLivePuller ) {
        pulling.store( false );
        puller.join();
        r.live = live;
    } else {
        // Pull everything the ring still holds, after the fact.
        std::vector<std::vector<float> > planar( channels,
                                                 std::vector<float>( 1024, 0.0f ) );
        std::vector<float *> ptrs( channels );
        for( std::uint32_t c = 0; c < channels; ++c ) ptrs[ c ] = planar[ c ].data();
        for( ;; ) {
            const std::size_t got = bridge.pullLive( ptrs.data(), channels, 1024 );
            if( !got ) break;
            const std::size_t base = r.live.size();
            r.live.resize( base + got * channels );
            for( std::size_t i = 0; i < got; ++i )
                for( std::uint32_t c = 0; c < channels; ++c )
                    r.live[ base + i * channels + c ] = planar[ c ][ i ];
        }
    }

    r.stats = bridge.stats();
    input.closeDevice();
    return r;
}

// Compare a captured planar source, a written WAV and the live stream against
// the fixture. Returns the number of mismatching samples in each.
void checkAgainstFixture( const char *label,
                          const std::shared_ptr<twGrowingCaptureSource> &src,
                          const std::string &wavPath,
                          const std::vector<float> &live,
                          std::size_t frames, std::uint32_t channels )
{
    char msg[ 256 ];

    // --- the PAGES ---
    std::size_t pageDiff = 0;
    std::vector<float> ch( frames, 0.0f );
    for( std::uint32_t c = 0; c < channels; ++c ) {
        const length_t got = src->read( 0, ch.data(), (length_t) frames, (idx_t) c );
        if( got != (length_t) frames ) { pageDiff += frames; continue; }
        for( std::size_t f = 0; f < frames; ++f )
            if( ch[ f ] != (float) fixtureSample( f, c ) / 32768.0f ) ++pageDiff;
    }
    std::snprintf( msg, sizeof msg,
                   "%s: pages equal the input file sample-for-sample "
                   "(%llu frames x %u ch, %llu differences)",
                   label, (unsigned long long) frames, (unsigned) channels,
                   (unsigned long long) pageDiff );
    CHECK( pageDiff == 0 && src->frontier() == frames, msg );

    // --- the WAV ---
    std::vector<std::int16_t> pcm;
    std::uint32_t wch = 0, wrate = 0;
    const bool readOk = readPcm16Wav( wavPath, pcm, wch, wrate );
    std::size_t wavDiff = 0;
    if( !readOk || wch != channels || pcm.size() != frames * channels ) {
        wavDiff = frames * channels;
    } else {
        for( std::size_t f = 0; f < frames; ++f )
            for( std::uint32_t c = 0; c < channels; ++c )
                if( pcm[ f * channels + c ] != fixtureSample( f, c ) ) ++wavDiff;
    }
    std::snprintf( msg, sizeof msg,
                   "%s: the written WAV equals the input file sample-for-sample "
                   "(%u ch @ %u Hz, %llu samples, %llu differences)",
                   label, (unsigned) wch, (unsigned) wrate,
                   (unsigned long long) pcm.size(), (unsigned long long) wavDiff );
    CHECK( readOk && wavDiff == 0, msg );

    // --- the LIVE lane ---
    std::size_t liveDiff = 0;
    if( live.size() != frames * channels ) {
        liveDiff = frames * channels;
    } else {
        for( std::size_t f = 0; f < frames; ++f )
            for( std::uint32_t c = 0; c < channels; ++c )
                if( live[ f * channels + c ] != (float) fixtureSample( f, c ) / 32768.0f )
                    ++liveDiff;
    }
    std::snprintf( msg, sizeof msg,
                   "%s: the live-lane sink delivered the same stream "
                   "(%llu frames pulled, %llu differences)",
                   label, (unsigned long long)( live.size() / channels ),
                   (unsigned long long) liveDiff );
    CHECK( liveDiff == 0, msg );
}

// ---------------------------------------------------------------------------
// (a) twGrowingCaptureSource as a data structure.
// ---------------------------------------------------------------------------
void testGrowingSource()
{
    std::printf( "\n-- twGrowingCaptureSource --\n" );

    const idx_t CH = 3;
    const std::size_t CHUNK = 64;
    twGrowingCaptureSource src( CH, 48000, CHUNK, 8 );   // 8 chunks == 512 frames

    CHECK( src.frontier() == 0 && src.length() == 0 && src.channels() == CH &&
           src.sampleRate() == 48000 && !src.isReproducible(),
           "growing: an empty source is length 0, width 3, not reproducible" );

    // A read on an empty source is a SHORT READ of zeroes, never a wait.
    float probe[ 8 ] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    const length_t emptyGot = src.read( 0, probe, 8, 0 );
    bool zeroed = true;
    for( float v : probe ) if( v != 0.0f ) zeroed = false;
    CHECK( emptyGot == 0 && zeroed,
           "growing: a read past the frontier is a short read of silence" );

    // Odd-sized appends that straddle chunk boundaries.
    const std::size_t TOTAL = 300;
    std::vector<float> in;
    std::size_t written = 0;
    const std::size_t sizes[] = { 7, 64, 1, 100, 63, 65 };
    for( std::size_t s : sizes ) {
        const std::size_t n = std::min( s, TOTAL - written );
        in.assign( n * CH, 0.0f );
        for( std::size_t f = 0; f < n; ++f )
            for( idx_t c = 0; c < CH; ++c )
                in[ f * CH + c ] = (float)( written + f ) + 0.25f * (float) c;
        src.append( in.data(), n );
        written += n;
        if( written >= TOTAL ) break;
    }
    CHECK( src.frontier() == written,
           "growing: the frontier is the sum of the appends" );

    std::size_t diff = 0;
    std::vector<float> back( written, 0.0f );
    for( idx_t c = 0; c < CH; ++c ) {
        src.read( 0, back.data(), (length_t) written, c );
        for( std::size_t f = 0; f < written; ++f )
            if( back[ f ] != (float) f + 0.25f * (float) c ) ++diff;
    }
    CHECK( diff == 0,
           "growing: every channel reads back exactly across chunk boundaries" );

    // A read that starts inside the material and runs past the frontier.
    std::vector<float> tail( 40, -1.0f );
    const length_t got = src.read( (offset_t)( written - 10 ), tail.data(), 40, 1 );
    bool tailOk = ( got == 10 );
    for( std::size_t i = 0; i < 10 && tailOk; ++i )
        tailOk = ( tail[ i ] == (float)( written - 10 + i ) + 0.25f );
    for( std::size_t i = 10; i < 40 && tailOk; ++i ) tailOk = ( tail[ i ] == 0.0f );
    CHECK( tailOk, "growing: a read crossing the frontier is short and zero-padded" );

    // Interleaved read with a channel mask (bits 0 and 2).
    std::vector<float> il( 20 * 2, -1.0f );
    const std::size_t ilGot = src.readInterleaved( 5, il.data(), 20, 0x5 );
    bool ilOk = ( ilGot == 20 && src.maskedChannels( 0x5 ) == 2 );
    for( std::size_t f = 0; f < 20 && ilOk; ++f ) {
        ilOk = il[ f * 2 + 0 ] == (float)( 5 + f ) &&
               il[ f * 2 + 1 ] == (float)( 5 + f ) + 0.5f;
    }
    CHECK( ilOk, "growing: readInterleaved honours the channel mask" );

    // The index is finite and says so instead of growing without bound.
    std::vector<float> big( 400 * CH, 2.0f );
    const std::size_t acc = src.append( big.data(), 400 );
    CHECK( acc == 512 - written && src.droppedFrames() == 400 - acc,
           "growing: appending past the chunk index is refused and counted" );

    // The one-copy handover.
    auto fixed = src.toCapturingSource( 0, 100 );
    std::vector<float> fb( 100, 0.0f );
    fixed->read( 0, fb.data(), 100, 2 );
    bool handoverOk = ( fixed->length() == 100 && fixed->channels() == CH &&
                        fixed->sampleRate() == 48000 && fixed->isReproducible() );
    for( std::size_t f = 0; f < 100 && handoverOk; ++f )
        handoverOk = ( fb[ f ] == (float) f + 0.5f );
    CHECK( handoverOk,
           "growing: toCapturingSource hands the storage to a fixed-size source" );
}

}  // namespace

int main()
{
    // A private scratch directory beside the binary is enough: everything this
    // test writes it also reads back itself.
    gTmpDir = ".";
    const char *outDir = std::getenv( "TW_RECORD_TEST_DIR" );
    if( outDir && *outDir ) gTmpDir = outDir;

    testGrowingSource();

    const std::size_t FRAMES = 32768;      // 32 blocks of 1024 — a whole number
    const int RATE = 48000;

    // ---------------------------------------------------------------------
    // (b) + (d) + (e) stereo: sample-exact pages/WAV/live, plus a second file
    //     carrying only channel 0.
    // ---------------------------------------------------------------------
    {
        std::printf( "\n-- 2 channels, real writer, concurrent live puller --\n" );
        const std::string fx = tmpPath( "rb_fixture_2ch.wav" );
        const std::string outAll = tmpPath( "rb_out_2ch.wav" );
        const std::string outL   = tmpPath( "rb_out_2ch_left.wav" );
        CHECK( writeFixtureWav( fx, RATE, 2, FRAMES ), "2ch: fixture written" );

        std::vector<audio::CaptureWavSink> sinks;
        sinks.push_back( { outAll, 0u } );
        sinks.push_back( { outL,   0x1u } );

        std::shared_ptr<twGrowingCaptureSource> src;
        RunResult r = runCapture( fx, FRAMES, 2, sinks, 0, true, src );
        CHECK( r.started && src, "2ch: the bridge ran" );
        if( r.started && src ) {
            checkAgainstFixture( "2ch", src, outAll, r.live, FRAMES, 2 );

            std::printf( "     in=%llu pages=%llu live=%llu wav=%llu "
                         "lateHW=%llu final=%llu ringOver=%llu liveOver=%llu\n",
                         (unsigned long long) r.stats.framesIn,
                         (unsigned long long) r.stats.framesToPages,
                         (unsigned long long) r.stats.framesToLive,
                         (unsigned long long) r.stats.framesToWav,
                         (unsigned long long) r.stats.wavLate,
                         (unsigned long long) r.stats.wavFinalized,
                         (unsigned long long) r.stats.ringOverruns,
                         (unsigned long long) r.stats.liveOverruns );

            CHECK( r.stats.framesIn == FRAMES && r.stats.framesToPages == FRAMES &&
                   r.stats.framesToWav == FRAMES && r.stats.framesToLive == FRAMES,
                   "2ch: every counter accounts for exactly the file's frames" );
            CHECK( r.stats.ringOverruns == 0 && r.stats.liveOverruns == 0 &&
                   r.stats.pageDrops == 0,
                   "2ch: nothing was dropped anywhere" );

            // The masked sink: one file, one channel, channel 0's audio.
            std::vector<std::int16_t> pcm;
            std::uint32_t wch = 0, wrate = 0;
            const bool ok = readPcm16Wav( outL, pcm, wch, wrate );
            std::size_t d = 0;
            if( !ok || wch != 1 || pcm.size() != FRAMES ) d = FRAMES;
            else for( std::size_t f = 0; f < FRAMES; ++f )
                if( pcm[ f ] != fixtureSample( f, 0 ) ) ++d;
            CHECK( ok && d == 0,
                   "2ch: the channel-masked sink wrote a mono file of channel 0" );
        }
    }

    // ---------------------------------------------------------------------
    // (c) the backpressure claim.
    // ---------------------------------------------------------------------
    {
        std::printf( "\n-- 2 channels, DELIBERATELY SLOW writer --\n" );
        const std::string fx  = tmpPath( "rb_fixture_slow.wav" );
        const std::string out = tmpPath( "rb_out_slow.wav" );
        CHECK( writeFixtureWav( fx, RATE, 2, FRAMES ), "slow: fixture written" );

        std::vector<audio::CaptureWavSink> sinks;
        sinks.push_back( { out, 0u } );

        std::shared_ptr<twGrowingCaptureSource> src;
        // 150 ms per 4096-frame write == ~27 frames/ms against a device
        // producing 48 frames/ms: the writer cannot possibly keep up.
        RunResult r = runCapture( fx, FRAMES, 2, sinks, 150, false, src );
        CHECK( r.started && src, "slow: the bridge ran" );
        if( r.started && src ) {
            std::printf( "     in=%llu pages=%llu wav=%llu lateHW=%llu "
                         "final=%llu ringOver=%llu\n",
                         (unsigned long long) r.stats.framesIn,
                         (unsigned long long) r.stats.framesToPages,
                         (unsigned long long) r.stats.framesToWav,
                         (unsigned long long) r.stats.wavLate,
                         (unsigned long long) r.stats.wavFinalized,
                         (unsigned long long) r.stats.ringOverruns );

            CHECK( r.stats.ringOverruns == 0,
                   "slow: a stalled WAV sink causes NO input-ring overrun" );
            CHECK( r.stats.framesIn == FRAMES && r.stats.framesToPages == FRAMES,
                   "slow: the pages still hold every captured frame" );
            CHECK( r.stats.wavLate >= 4096,
                   "slow: the backlog is reported (wavLate high-water >= one chunk)" );
            CHECK( r.stats.wavFinalized > 0,
                   "slow: the file was completed FROM THE PAGES at stop" );
            CHECK( r.stats.framesToWav == FRAMES,
                   "slow: every frame still reached the file" );
            checkAgainstFixture( "slow", src, out, r.live, FRAMES, 2 );
        }
    }

    // ---------------------------------------------------------------------
    // (b) wide: 6 channels.
    // ---------------------------------------------------------------------
    {
        std::printf( "\n-- 6 channels --\n" );
        const std::string fx  = tmpPath( "rb_fixture_6ch.wav" );
        const std::string out = tmpPath( "rb_out_6ch.wav" );
        CHECK( writeFixtureWav( fx, RATE, 6, FRAMES ), "6ch: fixture written" );

        std::vector<audio::CaptureWavSink> sinks;
        sinks.push_back( { out, 0u } );

        std::shared_ptr<twGrowingCaptureSource> src;
        RunResult r = runCapture( fx, FRAMES, 6, sinks, 0, false, src );
        CHECK( r.started && src, "6ch: the bridge ran" );
        if( r.started && src ) {
            CHECK( src->channels() == 6,
                   "6ch: the growing source took the device's width" );
            checkAgainstFixture( "6ch", src, out, r.live, FRAMES, 6 );
            CHECK( r.stats.ringOverruns == 0 && r.stats.pageDrops == 0,
                   "6ch: nothing was dropped" );
        }
    }

    // ---------------------------------------------------------------------
    // (f) RecordingSession as a bridge consumer, over the env selection.
    // ---------------------------------------------------------------------
    {
        std::printf( "\n-- RecordingSession over SMARAGD_AUDIO_INPUT_BACKEND=file: --\n" );
        const std::string fx = tmpPath( "rb_fixture_session.wav" );
        CHECK( writeFixtureWav( fx, RATE, 2, FRAMES ), "session: fixture written" );

        const std::string backend = "file:" + fx;
#if defined(_WIN32)
        _putenv_s( "SMARAGD_AUDIO_INPUT_BACKEND", backend.c_str() );
        _putenv_s( "SMARAGD_AUDIO_INPUT_LOOP", "0" );
#else
        setenv( "SMARAGD_AUDIO_INPUT_BACKEND", backend.c_str(), 1 );
        setenv( "SMARAGD_AUDIO_INPUT_LOOP", "0", 1 );
#endif

        audio::RecordingParams p;
        p.inputDeviceId = "default";
        p.armedTrackIds = { "trackA", "trackB" };
        p.trackChannels = { 0u, 0x1u };
        p.projectDirectory = gTmpDir;
        p.sampleRate = RATE;
        p.startLocatorFrames = 4800;

        std::atomic<std::uint64_t> lastPos{ 0 };
        audio::RecordingSession session;
        session.onPosition = [&lastPos]( std::uint64_t pos ) { lastPos.store( pos ); };

        const bool started = session.start( p );
        CHECK( started, "session: start() succeeded" );

        if( started ) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds( 20 );
            while( session.recordedDurationSeconds() * RATE < (double) FRAMES &&
                   std::chrono::steady_clock::now() < deadline )
                std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );

            session.requestStop();
            const auto fin = std::chrono::steady_clock::now() + std::chrono::seconds( 20 );
            while( !session.isFinished() && std::chrono::steady_clock::now() < fin )
                std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );

            CHECK( session.isFinished() && session.succeeded(),
                   "session: finished successfully" );
            CHECK( session.createdFiles().size() == 2,
                   "session: one file per armed track" );
            CHECK( lastPos.load() == 4800 + FRAMES,
                   "session: the playhead advanced from startLocatorFrames by the "
                   "captured project frames" );
            CHECK( session.bridge() != nullptr &&
                   session.bridge()->source() &&
                   session.bridge()->source()->frontier() == FRAMES,
                   "session: the capture pages survive the stop and hold the run" );

            if( session.createdFiles().size() == 2 ) {
                std::vector<std::int16_t> pcm;
                std::uint32_t wch = 0, wrate = 0;
                bool ok = readPcm16Wav( session.createdFiles()[ 0 ], pcm, wch, wrate );
                std::size_t d = ( ok && wch == 2 && pcm.size() == FRAMES * 2 ) ? 0
                                                                              : FRAMES * 2;
                if( !d )
                    for( std::size_t f = 0; f < FRAMES; ++f )
                        for( std::uint32_t c = 0; c < 2; ++c )
                            if( pcm[ f * 2 + c ] != fixtureSample( f, c ) ) ++d;
                CHECK( ok && d == 0 && wrate == (std::uint32_t) RATE,
                       "session: track A's file is the input, sample for sample" );

                ok = readPcm16Wav( session.createdFiles()[ 1 ], pcm, wch, wrate );
                d = ( ok && wch == 1 && pcm.size() == FRAMES ) ? 0 : FRAMES;
                if( !d )
                    for( std::size_t f = 0; f < FRAMES; ++f )
                        if( pcm[ f ] != fixtureSample( f, 0 ) ) ++d;
                CHECK( ok && d == 0,
                       "session: track B's file is channel 0 only" );
            }
        }

#if defined(_WIN32)
        _putenv_s( "SMARAGD_AUDIO_INPUT_BACKEND", "" );
        _putenv_s( "SMARAGD_AUDIO_INPUT_LOOP", "" );
#else
        unsetenv( "SMARAGD_AUDIO_INPUT_BACKEND" );
        unsetenv( "SMARAGD_AUDIO_INPUT_LOOP" );
#endif
    }

    std::printf( "\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
                 failures, failures == 1 ? "" : "s" );
    return failures ? 1 : 0;
}
