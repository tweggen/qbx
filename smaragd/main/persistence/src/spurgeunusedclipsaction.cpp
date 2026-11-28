#include "app/persistence/spurgeunusedclipsaction.h"

#include "app/actions/sactionregistry.h"
#include "app/model/sproject.h"
#include "app/model/sunusedclips.h"

#include "tw/core/twlog.h"

#include <QDomElement>
#include <QSet>

SPurgeUnusedClipsAction::SPurgeUnusedClipsAction( const QList<int> &rows,
                                                  bool all )
    : all_( all )
{
    QStringList parts;
    for( int r : rows ) parts << QString::number( r );
    rowsText_ = parts.join( QLatin1Char( ';' ) );
}

SApplyResult SPurgeUnusedClipsAction::apply( SProject *project )
{
    if( report_ ) *report_ = 0;
    if( !project ) return { false, nullptr };

    const QList<sunusedclips::Entry> entries = sunusedclips::list( project );
    if( entries.isEmpty() ) {
        TW_LOGW( "model", "purge-unused-clips: refused, the project "
                 "holds no unused clip" );
        return { false, nullptr };
    }
    const QString rowsSpec = rowsText_.trimmed();
    if( all_ == !rowsSpec.isEmpty() ) {
        TW_LOGW( "model", "purge-unused-clips: refused, give exactly "
                 "one of rows= and all=\"1\"" );
        return { false, nullptr };
    }

    // EVERY ROW IS CHECKED BEFORE ANYTHING IS RELEASED: a mistyped list is
    // refused whole, never purged in part.
    QList<int> rows;
    if( all_ ) {
        for( int i = 0; i < entries.size(); ++i ) rows << i;
    } else {
        QSet<int> seen;
        for( const QString &raw :
             rowsSpec.split( QLatin1Char( ';' ), Qt::SkipEmptyParts ) ) {
            bool ok = false;
            const int r = raw.trimmed().toInt( &ok );
            if( !ok || r < 0 || r >= entries.size() || seen.contains( r ) ) {
                TW_LOGW( "model", "purge-unused-clips: refused, row "
                         "\"%s\" is not one of the %d unused clip(s) (or is "
                         "repeated)", qPrintable( raw.trimmed() ),
                         int( entries.size() ) );
                return { false, nullptr };
            }
            seen.insert( r );
            rows << r;
        }
    }

    QList<SObject *> clips;
    QStringList labels;
    for( int r : rows ) {
        clips << entries.at( r ).clip;
        labels << entries.at( r ).label( project->getSRate() );
    }
    const int released = project->releaseLoadedOrphans( clips );
    if( report_ ) *report_ = released;
    TW_LOGI( "model", "purge-unused-clips: released %d of %d unused "
             "clip(s): %s", released, int( entries.size() ),
             qPrintable( labels.join( QStringLiteral( "; " ) ) ) );
    // NOT UNDOABLE: see the header for why an undo pin would make the purge
    // undo itself across a save.
    return { released > 0, nullptr };
}

void SPurgeUnusedClipsAction::writeXml( QDomElement &elem ) const
{
    if( !rowsText_.isEmpty() ) elem.setAttribute( "rows", rowsText_ );
    if( all_ ) elem.setAttribute( "all", "1" );
}

bool SPurgeUnusedClipsAction::readXml( const QDomElement &elem, int )
{
    rowsText_ = elem.attribute( "rows", "" );
    all_      = elem.attribute( "all", "0" ) != QLatin1String( "0" );
    return true;
}

static const bool s_reg_purge_unused_clips = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "purge-unused-clips" ),
        []{ return new SPurgeUnusedClipsAction; } ), true );
