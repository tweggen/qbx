#include "app/testkit/sassertsendsaction.h"

#include <QDebug>
#include <QDomElement>
#include <cmath>

#include "app/actions/sactionregistry.h"
#include "app/model/splacements.h"
#include "app/model/ssendtap.h"

namespace {
bool truthy( const QString &s )
{
    return s.startsWith( '1' ) || s.startsWith( 't' ) || s.startsWith( 'T' );
}
}  // namespace

SApplyResult SAssertSendsAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };

    QString rootName = arrangement_;
    SObject *root = nullptr;
    SObject *obj = splacements::laneBySpec( project, trackPath_, rootName, &root );
    if( !obj ) {
        qWarning().noquote() << "assert-sends FAILED:" << trackPath_
                             << "resolves to nothing";
        return { false, nullptr };
    }

    bool ok = true;
    auto fail = [&]( const QString &what, const QString &got,
                     const QString &want ) {
        qWarning().noquote() << "assert-sends FAILED [" << trackPath_ << "]"
                             << what << ": got" << got << "want" << want;
        ok = false;
    };

    const QList<SSendTap> &taps = obj->sendTaps();

    if( count_ >= 0 && taps.size() != count_ )
        fail( "count", QString::number( taps.size() ),
              QString::number( count_ ) );

    if( !dest_.isEmpty() ) {
        const SSendTap *t = obj->sendTap( dest_ );
        if( !absent_.isEmpty() && truthy( absent_ ) ) {
            if( t ) fail( "absent", QStringLiteral( "present" ),
                          QStringLiteral( "absent" ) );
        } else if( !t ) {
            fail( "dest", QStringLiteral( "absent" ), dest_ );
        } else {
            if( hasLevel_ && std::fabs( t->levelDb - level_ ) > tolerance_ )
                fail( "level", QString::number( t->levelDb, 'g', 10 ),
                      QString::number( level_, 'g', 10 ) );
            if( !pre_.isEmpty() && t->preFader != truthy( pre_ ) )
                fail( "pre", t->preFader ? "true" : "false", pre_ );
            if( !enabled_.isEmpty() && t->enabled != truthy( enabled_ ) )
                fail( "enabled", t->enabled ? "true" : "false", enabled_ );
        }
    }

    if( ok ) {
        QString shape;
        for( const SSendTap &t : taps ) {
            if( !shape.isEmpty() ) shape += QLatin1String( ", " );
            shape += QStringLiteral( "%1 %2dB %3%4" )
                         .arg( t.dest )
                         .arg( t.levelDb )
                         .arg( t.preFader ? "pre" : "post" )
                         .arg( t.enabled ? "" : " (off)" );
        }
        qInfo().noquote() << "assert-sends OK [" << trackPath_ << "] "
                          << taps.size() << "tap(s):"
                          << ( shape.isEmpty() ? QStringLiteral( "-" ) : shape );
    }
    return { ok, nullptr };
}

void SAssertSendsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "track", trackPath_ );
    if( count_ >= 0 )               elem.setAttribute( "count", count_ );
    if( !dest_.isEmpty() )          elem.setAttribute( "dest", dest_ );
    if( hasLevel_ )                 elem.setAttribute( "level", level_ );
    if( !pre_.isEmpty() )           elem.setAttribute( "pre", pre_ );
    if( !enabled_.isEmpty() )       elem.setAttribute( "enabled", enabled_ );
    if( !absent_.isEmpty() )        elem.setAttribute( "absent", absent_ );
    if( !arrangement_.isEmpty() )   elem.setAttribute( "arrangement", arrangement_ );
}

bool SAssertSendsAction::readXml( const QDomElement &elem, int )
{
    trackPath_   = elem.attribute( "track" );
    arrangement_ = elem.attribute( "arrangement" );
    dest_        = elem.attribute( "dest" );
    pre_         = elem.attribute( "pre" );
    enabled_     = elem.attribute( "enabled" );
    absent_      = elem.attribute( "absent" );
    count_       = elem.hasAttribute( "count" )
                       ? elem.attribute( "count" ).toInt() : -1;
    if( elem.hasAttribute( "level" ) ) {
        level_ = elem.attribute( "level" ).toDouble();
        hasLevel_ = true;
    }
    if( elem.hasAttribute( "tolerance" ) )
        tolerance_ = elem.attribute( "tolerance" ).toDouble();
    return !trackPath_.isEmpty();
}

static const bool s_reg_assert_sends = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-sends" ), []{ return new SAssertSendsAction; } ), true );
