#include "app/testkit/sarrangementtestactions.h"
#include "app/actions/sactionregistry.h"
#include "app/model/sproject.h"
#include "app/model/sobject.h"
#include "app/model/splacements.h"
#include <QDebug>
#include <QDomElement>
#include <QStringList>

// Resolve the root a verb addresses: the named arrangement, or the MASTER when
// the attribute is empty. Returns null (and warns) for an unknown name — never
// falling back to the master, which is the safety property the whole
// root-qualified scheme exists for (proposal 09 D21).
static SObject *rootFor( SProject *project, const QString &arrangement,
                         const char *who )
{
    if( !project ) {
        qWarning() << who << ": no project";
        return nullptr;
    }
    // splacements::rootNamed() IS this resolution (QBX-246), and spelling it
    // out here read as an assumption of the master while actually being the
    // correct rule. It also gains the ASSET branch, which the hand-written
    // version did not have: `rootNamed` resolves a fragment name too.
    SObject *root = splacements::rootNamed( project, arrangement );
    if( !root ) {
        if( arrangement.isEmpty() )
            qWarning() << who << ": the project has no master root";
        else
            qWarning() << who << ": no root named" << arrangement;
        return nullptr;
    }
    return root;
}

// --- assert-arrangements ----------------------------------------------------

SApplyResult SAssertArrangementsAction::apply( SProject *project )
{
    if( !project ) {
        qWarning() << "assert-arrangements: no project";
        return { false, nullptr };
    }

    QStringList expected = names_.split( ',', Qt::SkipEmptyParts );
    for( QString &e : expected ) e = e.trimmed();
    expected.sort();

    QStringList actual = project->arrangementNames();   // already sorted

    if( expected != actual ) {
        qWarning() << "assert-arrangements FAILED: expected" << expected
                   << "but got" << actual;
        return { false, nullptr };
    }

    if( !trackCounts_.isEmpty() ) {
        const QStringList counts = trackCounts_.split( ',', Qt::SkipEmptyParts );
        if( counts.size() != actual.size() ) {
            qWarning() << "assert-arrangements FAILED: trackCounts has"
                       << counts.size() << "entries for" << actual.size()
                       << "arrangements";
            return { false, nullptr };
        }
        for( int i = 0; i < actual.size(); ++i ) {
            SObject *root = project->arrangement( actual.at( i ) );
            const int have = root ? root->childCount() : -1;
            const int want = counts.at( i ).trimmed().toInt();
            if( have != want ) {
                qWarning() << "assert-arrangements FAILED:" << actual.at( i )
                           << "has" << have << "lanes, expected" << want;
                return { false, nullptr };
            }
        }
    }

    return { true, nullptr };
}

void SAssertArrangementsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "names", names_ );
    if( !trackCounts_.isEmpty() ) elem.setAttribute( "trackCounts", trackCounts_ );
}

bool SAssertArrangementsAction::readXml( const QDomElement &elem, int /*version*/ )
{
    names_       = elem.attribute( "names" );
    trackCounts_ = elem.attribute( "trackCounts" );
    return true;
}

// --- assert-track-count (root-aware, in-sequence) ---------------------------

SApplyResult SAssertArrangementTrackCountAction::apply( SProject *project )
{
    SObject *root = rootFor( project, arrangement_, "assert-track-count" );
    if( !root ) {
        return { false, nullptr };
    }
    const int actual = root->childCount();
    if( actual != count_ ) {
        qWarning() << "assert-track-count FAILED: arrangement"
                   << ( arrangement_.isEmpty() ? QStringLiteral( "<master>" )
                                               : arrangement_ )
                   << "has" << actual << "lanes, expected" << count_;
        return { false, nullptr };
    }
    return { true, nullptr };
}

void SAssertArrangementTrackCountAction::writeXml( QDomElement &elem ) const
{
    if( !arrangement_.isEmpty() ) elem.setAttribute( "arrangement", arrangement_ );
    elem.setAttribute( "count", count_ );
}

bool SAssertArrangementTrackCountAction::readXml( const QDomElement &elem,
                                                  int /*version*/ )
{
    arrangement_ = elem.attribute( "arrangement" );
    // No default: a case that forgets `count` must FAIL, not assert -1 == -1.
    count_ = elem.attribute( "count", "-1" ).toInt();
    return true;
}

// --- assert-ref-count -------------------------------------------------------

SApplyResult SAssertRefCountAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    if( arrangement_.isEmpty() == asset_.isEmpty() || refs_ < 0 ) {
        qWarning() << "assert-ref-count: give exactly one of arrangement/asset, and refs";
        return { false, nullptr };
    }
    SObject *o = arrangement_.isEmpty() ? project->asset( asset_ )
                                        : project->arrangement( arrangement_ );
    const QString what = arrangement_.isEmpty() ? asset_ : arrangement_;
    if( !o ) {
        qWarning() << "assert-ref-count FAILED: nothing registered as" << what;
        return { false, nullptr };
    }
    if( o->refCount() != refs_ ) {
        qWarning() << "assert-ref-count FAILED:" << what << "has" << o->refCount()
                   << "references, expected" << refs_;
        return { false, nullptr };
    }
    return { true, nullptr };
}

void SAssertRefCountAction::writeXml( QDomElement &elem ) const
{
    if( !arrangement_.isEmpty() ) elem.setAttribute( "arrangement", arrangement_ );
    if( !asset_.isEmpty() ) elem.setAttribute( "asset", asset_ );
    elem.setAttribute( "refs", refs_ );
}

bool SAssertRefCountAction::readXml( const QDomElement &elem, int /*version*/ )
{
    arrangement_ = elem.attribute( "arrangement" );
    asset_       = elem.attribute( "asset" );
    refs_        = elem.attribute( "refs", "-1" ).toInt();
    return true;
}

static const bool s_reg_arrangement_test = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-ref-count"),
        []{ return new SAssertRefCountAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-arrangements"),
        []{ return new SAssertArrangementsAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-track-count"),
        []{ return new SAssertArrangementTrackCountAction; } ),
    true
);
