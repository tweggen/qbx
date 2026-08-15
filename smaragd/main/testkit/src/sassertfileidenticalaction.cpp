#include "app/testkit/sassertfileidenticalaction.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"

#include <QDebug>
#include <QDir>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>

namespace {

// A file's path as this verb resolves it. See the header for the three rules;
// the ONLY thing that matters is that an absolute path survives untouched, so
// a golden that lives outside the output directory can be named at all.
QString resolvePath( const QString &spec )
{
    if( spec.isEmpty() ) return spec;
    if( QDir::isAbsolutePath( spec ) ) return spec;
    if( spec.contains( '/' ) || spec.contains( '\\' ) ) return spec;
    const QString outDir = SApplication::app().testOutputDir();
    return outDir.isEmpty() ? spec : ( outDir + "/" + spec );
}

// Minimal RIFF/WAVE geometry: where the sample data starts, how long it is,
// and how many bytes one frame takes. Deliberately hand-rolled and read-only —
// pulling in a decoder would decode, and this verb must compare the BYTES that
// were written, not a reinterpretation of them.
struct WavGeom {
    qint64  dataOffset = -1;
    qint64  dataBytes  = 0;
    quint16 channels   = 0;
    quint16 bits       = 0;
    quint32 rate       = 0;
    quint16 blockAlign = 0;
};

quint32 le32( const QByteArray &b, int at )
{
    return (quint32)(quint8) b[at]         | ( (quint32)(quint8) b[at+1] << 8 )
         | ( (quint32)(quint8) b[at+2] << 16 ) | ( (quint32)(quint8) b[at+3] << 24 );
}

quint16 le16( const QByteArray &b, int at )
{
    return (quint16)( (quint8) b[at] | ( (quint16)(quint8) b[at+1] << 8 ) );
}

bool parseWav( const QByteArray &buf, WavGeom &out, QString &error )
{
    if( buf.size() < 12 || buf.left( 4 ) != "RIFF" || buf.mid( 8, 4 ) != "WAVE" ) {
        error = QStringLiteral("not a RIFF/WAVE file");
        return false;
    }
    qint64 at = 12;
    while( at + 8 <= buf.size() ) {
        const QByteArray id = buf.mid( (int) at, 4 );
        const qint64 size = (qint64) le32( buf, (int) at + 4 );
        const qint64 body = at + 8;
        if( id == "fmt " && size >= 16 && body + 16 <= buf.size() ) {
            out.channels   = le16( buf, (int) body + 2 );
            out.rate       = le32( buf, (int) body + 4 );
            out.blockAlign = le16( buf, (int) body + 12 );
            out.bits       = le16( buf, (int) body + 14 );
        } else if( id == "data" ) {
            out.dataOffset = body;
            out.dataBytes  = qMin( size, (qint64) buf.size() - body );
        }
        at = body + size + ( size & 1 );   // chunks are word-aligned
    }
    if( out.dataOffset < 0 || out.blockAlign == 0 ) {
        error = QStringLiteral("no usable fmt/data chunk");
        return false;
    }
    return true;
}

bool readAll( const QString &path, QByteArray &out, QString &error )
{
    QFile f( path );
    if( !f.open( QIODevice::ReadOnly ) ) {
        error = QStringLiteral("cannot open %1").arg( path );
        return false;
    }
    out = f.readAll();
    f.close();
    return true;
}

}  // namespace

SAssertFileIdenticalAction::SAssertFileIdenticalAction( const QString &a,
                                                        const QString &b,
                                                        int64_t startFrame,
                                                        int64_t frameCount )
    : a_( a ), b_( b ), startFrame_( startFrame ), frameCount_( frameCount )
{
}

SApplyResult SAssertFileIdenticalAction::apply( SProject * /*project*/ )
{
    if( a_.isEmpty() || b_.isEmpty() ) {
        qWarning() << "SAssertFileIdenticalAction: both 'a' and 'b' are required";
        return { false, nullptr };
    }
    const QString pathA = resolvePath( a_ );
    const QString pathB = resolvePath( b_ );

    QByteArray bufA, bufB;
    QString error;
    if( !readAll( pathA, bufA, error ) || !readAll( pathB, bufB, error ) ) {
        qWarning() << "SAssertFileIdenticalAction:" << error;
        return { false, nullptr };
    }

    if( frameCount_ < 0 && startFrame_ == 0 ) {
        // Whole file, headers included — the `cmp` this verb exists to replace.
        if( bufA.size() != bufB.size() ) {
            qWarning() << "SAssertFileIdenticalAction: sizes differ —" << pathA
                       << bufA.size() << "bytes vs" << pathB << bufB.size()
                       << "bytes";
            return { false, nullptr };
        }
        if( bufA != bufB ) {
            qint64 firstDiff = -1;
            for( qint64 i = 0; i < bufA.size(); ++i ) {
                if( bufA[(int) i] != bufB[(int) i] ) { firstDiff = i; break; }
            }
            qWarning() << "SAssertFileIdenticalAction: files differ at byte"
                       << firstDiff << "—" << pathA << "vs" << pathB;
            return { false, nullptr };
        }
        qDebug() << "SAssertFileIdenticalAction: OK —" << pathA << "=="
                 << pathB << "(" << bufA.size() << "bytes )";
        return { true, nullptr };
    }

    // A frame range: compare the sample data only, so a case can assert about
    // a region without asserting about the header or the rest of the file.
    WavGeom ga, gb;
    if( !parseWav( bufA, ga, error ) ) {
        qWarning() << "SAssertFileIdenticalAction:" << pathA << error;
        return { false, nullptr };
    }
    if( !parseWav( bufB, gb, error ) ) {
        qWarning() << "SAssertFileIdenticalAction:" << pathB << error;
        return { false, nullptr };
    }
    if( ga.channels != gb.channels || ga.bits != gb.bits
        || ga.rate != gb.rate || ga.blockAlign != gb.blockAlign ) {
        qWarning() << "SAssertFileIdenticalAction: formats differ —" << pathA
                   << ga.channels << "ch" << ga.bits << "bit" << ga.rate << "Hz vs"
                   << pathB << gb.channels << "ch" << gb.bits << "bit"
                   << gb.rate << "Hz";
        return { false, nullptr };
    }

    const qint64 frameBytes = ga.blockAlign;
    const qint64 framesA = ga.dataBytes / frameBytes;
    const qint64 framesB = gb.dataBytes / frameBytes;
    const qint64 first = startFrame_ < 0 ? 0 : (qint64) startFrame_;
    const qint64 want = frameCount_ < 0 ? qMin( framesA, framesB ) - first
                                        : (qint64) frameCount_;
    if( want <= 0 ) {
        qWarning() << "SAssertFileIdenticalAction: empty frame range ( start"
                   << first << "count" << want << ")";
        return { false, nullptr };
    }
    if( first + want > framesA || first + want > framesB ) {
        qWarning() << "SAssertFileIdenticalAction: range [" << first << ","
                   << ( first + want ) << ") is past the end —" << pathA
                   << framesA << "frames," << pathB << framesB << "frames";
        return { false, nullptr };
    }

    const char *pa = bufA.constData() + ga.dataOffset + first * frameBytes;
    const char *pb = bufB.constData() + gb.dataOffset + first * frameBytes;
    const qint64 nBytes = want * frameBytes;
    if( memcmp( pa, pb, (size_t) nBytes ) != 0 ) {
        qint64 firstDiff = 0;
        while( firstDiff < nBytes && pa[firstDiff] == pb[firstDiff] ) ++firstDiff;
        qWarning() << "SAssertFileIdenticalAction: sample data differs at frame"
                   << ( first + firstDiff / frameBytes ) << "(byte" << firstDiff
                   << "of the compared range) —" << pathA << "vs" << pathB;
        return { false, nullptr };
    }

    qDebug() << "SAssertFileIdenticalAction: OK —" << want << "frames from"
             << first << "identical (" << pathA << "vs" << pathB << ")";
    return { true, nullptr };   // assertions are not undoable
}

void SAssertFileIdenticalAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "a", a_ );
    elem.setAttribute( "b", b_ );
    if( startFrame_ != 0 ) {
        elem.setAttribute( "startFrame", QString::number( startFrame_ ) );
    }
    if( frameCount_ != -1 ) {
        elem.setAttribute( "frameCount", QString::number( frameCount_ ) );
    }
}

bool SAssertFileIdenticalAction::readXml( const QDomElement &elem, int /*version*/ )
{
    // Missing paths are reported by apply(), not here: a readXml that fails
    // makes the action undeserializable, which breaks the round-trip audit
    // (it feeds every verb a DEFAULT instance through write -> read -> write).
    a_ = elem.attribute( "a", "" );
    b_ = elem.attribute( "b", "" );
    bool ok1 = false, ok2 = false;
    startFrame_ = elem.attribute( "startFrame", "0" ).toLongLong( &ok1 );
    frameCount_ = elem.attribute( "frameCount", "-1" ).toLongLong( &ok2 );
    if( !ok1 || !ok2 ) {
        qWarning() << "SAssertFileIdenticalAction::readXml: invalid numeric "
                      "attributes";
        return false;
    }
    return true;
}

static const bool s_reg_assert_file_identical = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-file-identical"),
        []{ return new SAssertFileIdenticalAction; }
    ), true
);
