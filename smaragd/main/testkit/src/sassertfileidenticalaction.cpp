#include "app/testkit/sassertfileidenticalaction.h"
#include "app/testkit/stestfilepath.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"

#include <QDebug>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>

SAssertFileIdenticalAction::SAssertFileIdenticalAction( const QString &actual,
                                                        const QString &expected,
                                                        int maxReportedDiffs,
                                                        qint64 startFrame,
                                                        qint64 frameCount )
    : actual_( actual ), expected_( expected ), maxReportedDiffs_( maxReportedDiffs ),
      startFrame_( startFrame ), frameCount_( frameCount )
{
}

namespace {

// Minimal RIFF/WAVE geometry (proposal 37 P0a): where the sample data starts,
// how long it is, and how many bytes one frame takes. Deliberately hand-rolled
// and read-only - pulling in a decoder would decode, and this verb must compare
// the BYTES that were written, not a reinterpretation of them.
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

// Read whole files. A rendered WAV in this suite is a few hundred KB to a few
// MB; a streaming compare in fixed chunks buys nothing there and would make
// the "how many bytes differ in total" figure harder to get right, which is the
// part of the diagnosis a hash could not give.
bool readAll( const QString &path, QByteArray &out, QString &err )
{
    QFile f( path );
    if( !f.open( QIODevice::ReadOnly ) ) {
        err = QStringLiteral( "cannot open %1 (%2)" ).arg( path, f.errorString() );
        return false;
    }
    out = f.readAll();
    f.close();
    return true;
}

}  // namespace

SApplyResult SAssertFileIdenticalAction::apply( SProject *project )
{
    if( actual_.isEmpty() || expected_.isEmpty() ) {
        qWarning() << "SAssertFileIdenticalAction: both actual= and expected= are required";
        return { false, nullptr };
    }

    SApplication &app = SApplication::app();
    const QString outputDir = app.testOutputDir();
    if( outputDir.isEmpty() ) {
        qWarning() << "SAssertFileIdenticalAction: no test output directory configured";
        return { false, nullptr };
    }

    QStringList triedA, triedE;
    const QString pathA = resolveTestFilePath( actual_,   outputDir, project, &triedA );
    const QString pathE = resolveTestFilePath( expected_, outputDir, project, &triedE );

    QByteArray a, e;
    QString err;
    if( !readAll( pathA, a, err ) ) {
        qWarning() << "SAssertFileIdenticalAction: actual:" << err
                   << "- tried" << triedA;
        return { false, nullptr };
    }
    if( !readAll( pathE, e, err ) ) {
        // A missing REFERENCE is a different failure from a missing render, and
        // the one place this verb gets used wrong: naming a reference that was
        // never produced would otherwise read as "the render is wrong".
        qWarning() << "SAssertFileIdenticalAction: expected (the reference):" << err
                   << "- tried" << triedE;
        return { false, nullptr };
    }

    // A frame range: compare the sample DATA of that range only (37 P0a).
    if( frameCount_ >= 0 || startFrame_ != 0 ) {
        return compareWavRange_( a, e );
    }

    const qint64 nA = a.size();
    const qint64 nE = e.size();
    const qint64 nCommon = qMin( nA, nE );

    qint64 firstDiff = -1;
    qint64 nDiff = 0;
    QStringList offsets;
    for( qint64 i = 0; i < nCommon; ++i ) {
        if( a.at( i ) != e.at( i ) ) {
            if( firstDiff < 0 ) firstDiff = i;
            ++nDiff;
            if( offsets.size() < maxReportedDiffs_ ) {
                offsets << QStringLiteral( "%1:%2!=%3" )
                               .arg( i )
                               .arg( (quint8) a.at( i ), 2, 16, QChar( '0' ) )
                               .arg( (quint8) e.at( i ), 2, 16, QChar( '0' ) );
            }
        }
    }

    if( nA == nE && nDiff == 0 ) {
        qDebug() << "SAssertFileIdenticalAction: OK —" << actual_ << "is byte-identical to"
                 << expected_ << QStringLiteral( "(%1 bytes)" ).arg( nA );
        return { true, nullptr };   // assertions are not undoable
    }

    // Everything below is the diagnosis. Report the size difference AND the
    // content difference: a truncated render and a re-rendered one are both
    // "not identical" and have nothing else in common.
    QString msg = QStringLiteral( "%1 (%2 B) differs from %3 (%4 B)" )
                      .arg( actual_ ).arg( nA ).arg( expected_ ).arg( nE );

    if( nA != nE ) {
        msg += QStringLiteral( "; SIZE differs by %1 B" ).arg( nA - nE );
    }
    if( firstDiff >= 0 ) {
        msg += QStringLiteral( "; first differing byte at offset %1 (0x%2): %3 vs %4" )
                   .arg( firstDiff )
                   .arg( firstDiff, 0, 16 )
                   .arg( (quint8) a.at( firstDiff ) )
                   .arg( (quint8) e.at( firstDiff ) );
        msg += QStringLiteral( "; %1 of %2 common bytes differ (%3%)" )
                   .arg( nDiff ).arg( nCommon )
                   .arg( nCommon ? ( 100.0 * (double) nDiff / (double) nCommon ) : 0.0,
                         0, 'f', 3 );
        if( !offsets.isEmpty() ) {
            msg += QStringLiteral( "; first offsets [%1]" ).arg( offsets.join( ' ' ) );
        }
    } else {
        msg += QStringLiteral( "; the common %1 bytes are IDENTICAL (a pure "
                               "length difference)" ).arg( nCommon );
    }

    qWarning() << "SAssertFileIdenticalAction:" << msg;
    return { false, nullptr };
}

SApplyResult SAssertFileIdenticalAction::compareWavRange_( const QByteArray &a,
                                                           const QByteArray &e )
{
    WavGeom ga, ge;
    QString err;
    if( !parseWav( a, ga, err ) ) {
        qWarning() << "SAssertFileIdenticalAction: actual:" << actual_ << err;
        return { false, nullptr };
    }
    if( !parseWav( e, ge, err ) ) {
        qWarning() << "SAssertFileIdenticalAction: expected:" << expected_ << err;
        return { false, nullptr };
    }
    if( ga.channels != ge.channels || ga.bits != ge.bits || ga.rate != ge.rate
        || ga.blockAlign != ge.blockAlign ) {
        qWarning() << "SAssertFileIdenticalAction: formats differ -"
                   << actual_ << QStringLiteral( "%1ch/%2bit/%3Hz" )
                                    .arg( ga.channels ).arg( ga.bits ).arg( ga.rate )
                   << "vs" << expected_
                   << QStringLiteral( "%1ch/%2bit/%3Hz" )
                          .arg( ge.channels ).arg( ge.bits ).arg( ge.rate );
        return { false, nullptr };
    }
    const qint64 frameBytes = ga.blockAlign;
    const qint64 framesA = ga.dataBytes / frameBytes;
    const qint64 framesE = ge.dataBytes / frameBytes;
    const qint64 first = startFrame_ < 0 ? 0 : startFrame_;
    const qint64 want = frameCount_ < 0 ? qMin( framesA, framesE ) - first : frameCount_;
    if( want < 0 || first + want > framesA || first + want > framesE ) {
        qWarning() << "SAssertFileIdenticalAction: range" << first << "+" << want
                   << "exceeds the data:" << actual_ << "has" << framesA
                   << "frames," << expected_ << "has" << framesE;
        return { false, nullptr };
    }
    const char *pa = a.constData() + ga.dataOffset + first * frameBytes;
    const char *pe = e.constData() + ge.dataOffset + first * frameBytes;
    const qint64 nBytes = want * frameBytes;
    qint64 firstDiff = -1, nDiff = 0;
    for( qint64 i = 0; i < nBytes; ++i ) {
        if( pa[i] != pe[i] ) {
            if( firstDiff < 0 ) firstDiff = i;
            ++nDiff;
        }
    }
    if( nDiff == 0 ) {
        qDebug() << "SAssertFileIdenticalAction: OK -" << actual_ << "and" << expected_
                 << "are identical over frames" << first << "+" << want;
        return { true, nullptr };
    }
    qWarning() << "SAssertFileIdenticalAction:" << actual_ << "differs from" << expected_
               << "in the sample data: first difference at frame"
               << ( first + firstDiff / frameBytes )
               << QStringLiteral( "(%1 of %2 bytes in the range differ)" )
                      .arg( nDiff ).arg( nBytes );
    return { false, nullptr };
}

void SAssertFileIdenticalAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "actual", actual_ );
    elem.setAttribute( "expected", expected_ );
    if( maxReportedDiffs_ != 8 ) {
        elem.setAttribute( "maxReportedDiffs", QString::number( maxReportedDiffs_ ) );
    }
    if( startFrame_ != 0 ) {
        elem.setAttribute( "startFrame", QString::number( startFrame_ ) );
    }
    if( frameCount_ >= 0 ) {
        elem.setAttribute( "frameCount", QString::number( frameCount_ ) );
    }
}

bool SAssertFileIdenticalAction::readXml( const QDomElement &elem, int /*version*/ )
{
    // Missing attributes are reported by apply(), not here: readXml failing
    // makes the action undeserializable, which breaks the round-trip audit
    // (it feeds every action a DEFAULT instance through write->read->write).
    actual_   = elem.attribute( "actual", "" );
    expected_ = elem.attribute( "expected", "" );

    bool ok = true;
    maxReportedDiffs_ = elem.attribute( "maxReportedDiffs", "8" ).toInt( &ok );
    if( !ok || maxReportedDiffs_ < 0 ) {
        qWarning() << "SAssertFileIdenticalAction::readXml: invalid maxReportedDiffs";
        return false;
    }
    startFrame_ = elem.attribute( "startFrame", "0" ).toLongLong( &ok );
    if( !ok ) return false;
    frameCount_ = elem.attribute( "frameCount", "-1" ).toLongLong( &ok );
    if( !ok ) return false;
    return true;
}

static const bool s_reg_assert_file_identical = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-file-identical"),
        []{ return new SAssertFileIdenticalAction; }
    ), true
);
