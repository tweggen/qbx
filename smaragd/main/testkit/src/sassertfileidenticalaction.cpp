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
                                                        int maxReportedDiffs )
    : actual_( actual ), expected_( expected ), maxReportedDiffs_( maxReportedDiffs )
{
}

namespace {

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

void SAssertFileIdenticalAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "actual", actual_ );
    elem.setAttribute( "expected", expected_ );
    if( maxReportedDiffs_ != 8 ) {
        elem.setAttribute( "maxReportedDiffs", QString::number( maxReportedDiffs_ ) );
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
    return true;
}

static const bool s_reg_assert_file_identical = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-file-identical"),
        []{ return new SAssertFileIdenticalAction; }
    ), true
);
