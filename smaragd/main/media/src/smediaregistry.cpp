#include "app/media/smediaregistry.h"

#include "app/media/smediasource.h"

#include "tw/core/twlog.h"

#include <algorithm>

SMediaRegistry &SMediaRegistry::instance()
{
    static SMediaRegistry theRegistry;
    return theRegistry;
}

SMediaRegistry::SMediaRegistry( QObject *parent )
    : QObject( parent )
{
}

void SMediaRegistry::registerSource( SMediaSource *source )
{
    if( !source ) return;
    const QString sid = source->id();
    if( sid.isEmpty() ) {
        TW_LOGW( "media", "registry: refusing a source with an empty id" );
        return;
    }

    auto existing = sources_.constFind( sid );
    if( existing != sources_.constEnd() && existing.value() != source ) {
        TW_LOGW( "media", "registry: replacing the source registered as '%s'",
                 sid.toUtf8().constData() );
        disconnect( existing.value(), &QObject::destroyed, this, nullptr );
    } else if( existing != sources_.constEnd() ) {
        return;   // already registered, same object
    }

    sources_.insert( sid, source );
    // The registry does NOT own its sources; it forgets one that dies.
    connect( source, &QObject::destroyed,
             this, &SMediaRegistry::onSourceDestroyed );
    emit sourcesChanged();
}

void SMediaRegistry::unregisterSource( const QString &id )
{
    auto it = sources_.find( id );
    if( it == sources_.end() ) return;
    if( it.value() ) disconnect( it.value(), &QObject::destroyed, this, nullptr );
    sources_.erase( it );
    emit sourcesChanged();
}

SMediaSource *SMediaRegistry::source( const QString &id ) const
{
    return sources_.value( id, nullptr );
}

QStringList SMediaRegistry::sourceIds() const
{
    QStringList ids = sources_.keys();
    std::sort( ids.begin(), ids.end() );
    return ids;
}

int SMediaRegistry::count() const
{
    return sources_.size();
}

void SMediaRegistry::onSourceDestroyed( QObject *dead )
{
    // destroyed() fires from ~QObject, where the SMediaSource slice is already
    // gone — so match on the POINTER, never by calling id() on it.
    bool changed = false;
    for( auto it = sources_.begin(); it != sources_.end(); ) {
        if( static_cast<QObject *>( it.value() ) == dead ) {
            it      = sources_.erase( it );
            changed = true;
        } else {
            ++it;
        }
    }
    if( changed ) emit sourcesChanged();
}
