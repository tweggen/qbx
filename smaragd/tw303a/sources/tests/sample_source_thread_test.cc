// QBX-305: twSampleSource decodes on a RAW std::thread without Qt adopting it.
//
// The sampler's generation job (SSamplerProgram::Job) decodes a dropped file
// with twSampleSource on a raw std::thread, and calls itself Qt-free
// (docs/contracts/THREADING.md rule 1). twSampleSource opened the file with a
// QFile -- a QObject -- so the first decode made Qt ADOPT the worker, and the
// adopted thread's Qt-TLS cleanup ran at DLL THREAD_DETACH when the worker
// exited: a SIGSEGV in Qt6Core.dll under _endthreadex on Windows.
//
// THE WITNESS. twQtFreeWatch hooks QObject construction (Qt's qtHookData
// table) and counts every QObject born on a thread inside a twQtFreeScope.
// Adoption itself constructs one -- the QAdoptedThread, on the adopting thread
// -- so the count sees adoption by any route, not only a QFile. Sections 1 and
// 2 prove that the witness bites before anything is concluded from a zero.
//
// Sections:
//   1  control: a QObject made inside a scope on a raw thread is counted
//   2  control: QThread::currentThread() alone (adoption, no object of ours)
//   3  16-bit WAV, the hand-rolled fast path (loadWav)
//   4  24-bit WAV, which loadWav refuses and libsndfile decodes (loadSndfile)
//   5  a file that does not exist (the error paths, which log)
//   6  a non-ASCII path loads, and decodes the same PCM as the ASCII one
//   7  logging from a raw thread: TW_LOG and qWarning (measured)

#include "tw/core/twqtfree.h"
#include "tw/core/twlog.h"
#include "tw/sources/twsamplesource.h"
#include "tw/graph/tw303aenv.h"

#include <QtCore/QByteArray>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK( cond, msg )                                                  \
    do {                                                                    \
        if( cond ) { printf( "ok   %s\n", msg ); }                          \
        else       { printf( "FAIL %s\n", msg ); ++failures; }              \
    } while( 0 )

// Run `work` on a FRESH raw std::thread inside a twQtFreeScope, and return the
// QObjects constructed there. A fresh thread each time: a thread Qt already
// adopted would not build a second QAdoptedThread.
static std::uint64_t onRawThread( const std::function<void()> &work )
{
    twQtFreeWatch::reset();
    std::thread t( [&] {
        twQtFreeScope scope;
        work();
    } );
    t.join();
    return twQtFreeWatch::violations();
}

// A 24-bit PCM WAV (mono, 48 kHz) of `frames` frames, a ramp. loadWav refuses
// anything but 16-bit, so this one reaches libsndfile.
static QByteArray wav24( int frames )
{
    QByteArray b;
    auto u32 = [&]( std::uint32_t v ) { for( int i = 0; i < 4; ++i ) b.append( char( ( v >> ( 8 * i ) ) & 0xff ) ); };
    auto u16 = [&]( std::uint16_t v ) { b.append( char( v & 0xff ) ); b.append( char( v >> 8 ) ); };
    const std::uint32_t dataLen = (std::uint32_t)frames * 3;
    b.append( "RIFF" ); u32( 36 + dataLen ); b.append( "WAVE" );
    b.append( "fmt " ); u32( 16 ); u16( 1 ); u16( 1 ); u32( 48000 ); u32( 48000 * 3 ); u16( 3 ); u16( 24 );
    b.append( "data" ); u32( dataLen );
    for( int f = 0; f < frames; ++f ) {
        const std::int32_t v = ( ( f % 2000 ) - 1000 ) * 4000;
        b.append( char( v & 0xff ) ); b.append( char( ( v >> 8 ) & 0xff ) ); b.append( char( ( v >> 16 ) & 0xff ) );
    }
    return b;
}

static bool writeFile( const QString &path, const QByteArray &bytes )
{
    QFile f( path );   // main thread: Qt owns this thread, a QFile is fine here
    return f.open( QIODevice::WriteOnly ) && f.write( bytes ) == bytes.size();
}

struct Loaded {
    bool          ok       = false;
    int           channels = 0;
    long long     frames   = 0;
    twContentHash hash;
};

static Loaded load( tw303aEnvironment &env, const QString &path )
{
    twSampleSource s( env, path );
    Loaded r;
    r.ok       = s.wasLoaded();
    r.channels = (int)s.channels();
    r.frames   = (long long)s.length();
    r.hash     = s.contentHash();
    return r;
}

int main()
{
    tw303aEnvironment env;

    const bool hooked = twQtFreeWatch::install();
    CHECK( hooked, "the QObject-construction hook is installed (else nothing below can see adoption)" );
    if( !hooked ) return 1;

    // --- 1. control: the witness counts a QObject made inside a scope ------
    {
        const std::uint64_t n = onRawThread( [] { QObject o; } );
        printf( "     control QObject on a raw thread: %llu QObject(s) constructed\n",
                (unsigned long long)n );
        CHECK( n >= 2, "a QObject inside a scope is counted, with the QAdoptedThread it brings (>= 2)" );
        CHECK( twQtFreeWatch::scopesEntered() == 1, "the scope was entered exactly once" );
    }

    // --- 2. control: adoption alone, no QObject of ours --------------------
    {
        const std::uint64_t n = onRawThread( [] { (void)QThread::currentThread(); } );
        printf( "     control QThread::currentThread() on a raw thread: %llu\n", (unsigned long long)n );
        CHECK( n == 1, "adoption alone is counted: exactly the QAdoptedThread" );
    }

    // --- 3. 16-bit WAV: loadWav ---------------------------------------------
    Loaded asciiWav;
    {
        const QString path = QString::fromUtf8( TW_MONO_FIXTURE );
        const std::uint64_t n = onRawThread( [&] { asciiWav = load( env, path ); } );
        printf( "     16-bit WAV on a raw thread: %llu QObject(s) constructed\n", (unsigned long long)n );
        CHECK( asciiWav.ok && asciiWav.channels == 1 && asciiWav.frames == 765952,
               "the 16-bit fixture decodes on a raw thread (1 ch, 765952 frames)" );
        CHECK( n == 0, "a 16-bit WAV load constructs NO QObject on a raw thread (not adopted)" );
    }

    QTemporaryDir tmp;
    CHECK( tmp.isValid(), "a temporary directory" );
    const QString w24 = QDir( tmp.path() ).filePath( QStringLiteral( "ramp24.wav" ) );
    CHECK( writeFile( w24, wav24( 48000 ) ), "the 24-bit fixture is written" );

    // --- 4. 24-bit WAV: loadWav refuses, loadSndfile decodes ----------------
    Loaded ascii24;
    {
        const std::uint64_t n = onRawThread( [&] { ascii24 = load( env, w24 ); } );
        printf( "     24-bit WAV on a raw thread: %llu QObject(s) constructed\n", (unsigned long long)n );
        CHECK( ascii24.ok && ascii24.channels == 1 && ascii24.frames == 48000,
               "the 24-bit fixture decodes through libsndfile on a raw thread (1 ch, 48000 frames)" );
        CHECK( n == 0, "a libsndfile load constructs NO QObject on a raw thread (not adopted)" );
    }

    // --- 5. a missing file: the error paths ---------------------------------
    {
        const QString missing = QDir( tmp.path() ).filePath( QStringLiteral( "absent.wav" ) );
        Loaded r;
        const std::uint64_t n = onRawThread( [&] { r = load( env, missing ); } );
        printf( "     missing file on a raw thread: %llu\n", (unsigned long long)n );
        CHECK( !r.ok, "a missing file does not load" );
        CHECK( n == 0, "a failed load constructs NO QObject on a raw thread either" );
    }

    // --- 6. a non-ASCII path -------------------------------------------------
    {
        // Latin-1 range, outside it, and a CJK character: a narrow fopen with
        // the ANSI code page on Windows cannot open the last two.
        const QString dir = QDir( tmp.path() ).filePath( QString::fromUtf8( "Kl\xc3\xa4nge \xce\xb1\xce\xb2 \xe9\x9f\xb3" ) );
        CHECK( QDir().mkpath( dir ), "the non-ASCII directory is created" );
        const QString p16 = QDir( dir ).filePath( QString::fromUtf8( "s\xc3\xbc\xc3\x9f_\xe2\x82\xac.wav" ) );
        const QString p24 = QDir( dir ).filePath( QString::fromUtf8( "r\xc3\xa4mp_\xe9\x9f\xb3.wav" ) );
        QFile src( QString::fromUtf8( TW_MONO_FIXTURE ) );
        CHECK( src.copy( p16 ), "the 16-bit fixture is copied to a non-ASCII path" );
        CHECK( writeFile( p24, wav24( 48000 ) ), "the 24-bit fixture is written to a non-ASCII path" );

        Loaded u16, u24, m16;
        const std::uint64_t n = onRawThread( [&] { u16 = load( env, p16 ); u24 = load( env, p24 ); } );
        m16 = load( env, p16 );   // and on the main thread
        CHECK( u16.ok && u16.frames == asciiWav.frames && u16.hash == asciiWav.hash,
               "a 16-bit WAV at a non-ASCII path decodes the same PCM as at an ASCII one" );
        CHECK( m16.ok && m16.hash == asciiWav.hash, "... on the main thread too" );
        CHECK( u24.ok && u24.frames == ascii24.frames && u24.hash == ascii24.hash,
               "a libsndfile load at a non-ASCII path decodes the same PCM as at an ASCII one" );
        CHECK( n == 0, "non-ASCII loads construct NO QObject on a raw thread" );
    }

    // --- 7. logging from a raw thread ---------------------------------------
    {
        const std::uint64_t nTw = onRawThread( [] { TW_LOGW( "test", "QBX-305 probe from a raw thread" ); } );
        printf( "     TW_LOGW on a raw thread: %llu\n", (unsigned long long)nTw );
        CHECK( nTw == 0, "TW_LOGW constructs no QObject on a raw thread" );
        // Measured, not asserted: whether Qt's own message path adopts a
        // thread is Qt's business and the app installs its own handler. The
        // engine logs through TwLog on worker threads either way.
        const std::uint64_t nQt = onRawThread( [] { qWarning( "QBX-305 probe from a raw thread" ); } );
        printf( "     qWarning on a raw thread (default handler, measured): %llu\n", (unsigned long long)nQt );
    }

    printf( "%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures );
    return failures ? 1 : 0;
}
