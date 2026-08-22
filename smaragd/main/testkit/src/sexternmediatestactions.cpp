#include "app/testkit/sexternmediatestactions.h"

#include "app/actions/sactionregistry.h"
#include "app/media/smediadrop.h"
#include "app/model/sexternfile.h"
#include "app/model/sproject.h"

#include <QDebug>
#include <QDomElement>
#include <QStringList>

// --- assert-extern-files ----------------------------------------------------

SApplyResult SAssertExternFilesAction::apply( SProject *project )
{
    if( !project ) {
        qWarning() << "assert-extern-files FAILED: no project";
        return { false, nullptr };
    }

    const QHash<QString,SExternFile*> &files = project->externFiles();
    int missing = 0;
    QStringList missingPaths;
    for( auto it = files.constBegin(); it != files.constEnd(); ++it ) {
        if( it.value() && it.value()->isMissing() ) {
            ++missing;
            missingPaths << it.key();
        }
    }
    const QStringList external = project->externalMediaPaths();

    bool ok = true;
    if( count_ >= 0 && files.size() != count_ ) {
        qWarning() << "assert-extern-files FAILED: count is" << files.size()
                   << "expected" << count_;
        ok = false;
    }
    if( missing_ >= 0 && missing != missing_ ) {
        qWarning() << "assert-extern-files FAILED: missing is" << missing
                   << "expected" << missing_ << "—" << missingPaths;
        ok = false;
    }
    if( external_ >= 0 && external.size() != external_ ) {
        qWarning() << "assert-extern-files FAILED: external is" << external.size()
                   << "expected" << external_ << "—" << external;
        ok = false;
    }
    if( !ok ) return { false, nullptr };

    qDebug() << "assert-extern-files: OK — count" << files.size()
             << "missing" << missing << "external" << external.size();
    return { true, nullptr };   // assertions are not undoable
}

void SAssertExternFilesAction::writeXml( QDomElement &elem ) const
{
    if( count_    >= 0 ) elem.setAttribute( "count",    count_ );
    if( missing_  >= 0 ) elem.setAttribute( "missing",  missing_ );
    if( external_ >= 0 ) elem.setAttribute( "external", external_ );
}

bool SAssertExternFilesAction::readXml( const QDomElement &elem, int /*version*/ )
{
    count_    = elem.attribute( "count",    "-1" ).toInt();
    missing_  = elem.attribute( "missing",  "-1" ).toInt();
    external_ = elem.attribute( "external", "-1" ).toInt();
    if( count_ < 0 && missing_ < 0 && external_ < 0 ) {
        qWarning() << "assert-extern-files::readXml: nothing to assert";
        return false;
    }
    return true;
}

static const bool s_reg_assert_extern_files = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-extern-files" ),
        []{ return new SAssertExternFilesAction; }
    ), true
);

// --- collect-external-media -------------------------------------------------

SApplyResult SCollectExternalMediaAction::apply( SProject *project )
{
    if( !project ) {
        qWarning() << "collect-external-media FAILED: no project";
        return { false, nullptr };
    }

    QStringList skippedMissing, failed;
    const int copied =
        smediadrop::collectExternalMedia( project, &skippedMissing, &failed );

    bool ok = true;
    if( expectCopied_ >= 0 && copied != expectCopied_ ) {
        qWarning() << "collect-external-media FAILED: copied" << copied
                   << "expected" << expectCopied_;
        ok = false;
    }
    if( expectMissing_ >= 0 && skippedMissing.size() != expectMissing_ ) {
        qWarning() << "collect-external-media FAILED: skipped-missing"
                   << skippedMissing.size() << "expected" << expectMissing_
                   << "—" << skippedMissing;
        ok = false;
    }
    if( expectFailed_ >= 0 && failed.size() != expectFailed_ ) {
        qWarning() << "collect-external-media FAILED: failed" << failed.size()
                   << "expected" << expectFailed_ << "—" << failed;
        ok = false;
    }
    if( !ok ) return { false, nullptr };

    qDebug() << "collect-external-media: OK — copied" << copied
             << "missing" << skippedMissing.size()
             << "failed" << failed.size();
    // NOT undoable, and that is the feature's own contract rather than a
    // testkit shortcut: the pass copies files, and no undo step can put a file
    // system back the way it found it.
    return { true, nullptr };
}

void SCollectExternalMediaAction::writeXml( QDomElement &elem ) const
{
    if( expectCopied_  >= 0 ) elem.setAttribute( "expectCopied",  expectCopied_ );
    if( expectMissing_ >= 0 ) elem.setAttribute( "expectMissing", expectMissing_ );
    if( expectFailed_  >= 0 ) elem.setAttribute( "expectFailed",  expectFailed_ );
}

bool SCollectExternalMediaAction::readXml( const QDomElement &elem, int /*version*/ )
{
    expectCopied_  = elem.attribute( "expectCopied",  "-1" ).toInt();
    expectMissing_ = elem.attribute( "expectMissing", "-1" ).toInt();
    expectFailed_  = elem.attribute( "expectFailed",  "-1" ).toInt();
    return true;
}

static const bool s_reg_collect_external_media = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "collect-external-media" ),
        []{ return new SCollectExternalMediaAction; }
    ), true
);
