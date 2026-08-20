#include "app/testkit/sarrangementtestactions.h"
#include "app/actions/sactionregistry.h"
#include "app/model/sproject.h"
#include "app/model/sobject.h"
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
    if( arrangement.isEmpty() ) return project->getRootComponent();
    SObject *root = project->arrangement( arrangement );
    if( !root ) {
        qWarning() << who << ": no arrangement named" << arrangement;
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

static const bool s_reg_arrangement_test = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-arrangements"),
        []{ return new SAssertArrangementsAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-track-count"),
        []{ return new SAssertArrangementTrackCountAction; } ),
    true
);
