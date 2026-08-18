#include "app/media/smediasource.h"

SMediaSource::SMediaSource( QObject *parent )
    : QObject( parent )
{
    // A batch crosses a queued connection, so its argument types must be
    // registered metatypes. Doing it here means no subclass and no caller has
    // to remember (an unregistered queued argument is dropped with a runtime
    // warning, which reads as "the walk found nothing").
    smedia::registerMediaMetaTypes();
}

SMediaSource::~SMediaSource() = default;

void SMediaSource::setSuffixFilter( const QStringList &suffixes )
{
    suffixFilter_ = suffixes;
}

QStringList SMediaSource::suffixFilter() const
{
    return suffixFilter_;
}

int SMediaSource::nextRequestId()
{
    return ++nextRequestId_;
}
