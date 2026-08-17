#include "app/testkit/sassertlogaction.h"
#include "app/actions/sactionregistry.h"
#include "tw/core/twlog.h"

#include <QDebug>
#include <QDomElement>

#include <atomic>
#include <vector>

namespace {

// Start of the window this verb reads. Plain atomic, not a member: the runner
// moves it, the action reads it, and both are on the main thread — the atomic
// is only so that nothing here is a data race if a future runner is not.
std::atomic<uint64_t> g_windowStart{ 0 };

bool parseLevel( const QString &spec, tw::LogLevel &out )
{
    const QString s = spec.trimmed().toLower();
    if( s == "error" || s == "err" )    { out = tw::LogLevel::Error; return true; }
    if( s == "warn"  || s == "warning" ){ out = tw::LogLevel::Warn;  return true; }
    if( s == "info" )                   { out = tw::LogLevel::Info;  return true; }
    if( s == "debug" )                  { out = tw::LogLevel::Debug; return true; }
    if( s == "trace" )                  { out = tw::LogLevel::Trace; return true; }
    return false;
}

}  // namespace

void SAssertLogAction::beginAction( const QString &verb )
{
    // An assert-log does not move the window: two of them in a row must both
    // examine the action they follow, not each other.
    if( verb == QLatin1String("assert-log") ) return;
    g_windowStart.store( tw::TwLog::instance().nextSeq() );
}

uint64_t SAssertLogAction::windowStart()
{
    return g_windowStart.load();
}

SAssertLogAction::SAssertLogAction( const QString &contains, int minCount,
                                    int maxCount, const QString &level )
    : contains_( contains ), minCount_( minCount ), maxCount_( maxCount ),
      level_( level )
{
}

SApplyResult SAssertLogAction::apply( SProject * /*project*/ )
{
    if( contains_.isEmpty() ) {
        qWarning() << "SAssertLogAction: 'contains' is required";
        return { false, nullptr };
    }
    tw::LogLevel wantLevel = tw::LogLevel::Trace;
    const bool filterLevel = !level_.isEmpty();
    if( filterLevel && !parseLevel( level_, wantLevel ) ) {
        qWarning() << "SAssertLogAction: unknown level" << level_
                   << "( expected error|warn|info|debug|trace )";
        return { false, nullptr };
    }

    tw::TwLog &log = tw::TwLog::instance();
    const uint64_t from = windowStart();
    const uint64_t to   = log.nextSeq();

    // Drained in chunks: the window can legitimately span a whole render, and
    // snapshot() copies the records it returns.
    const std::string needle = contains_.toStdString();
    std::vector<tw::LogRecord> batch;
    int found = 0;
    for( uint64_t at = from; at < to; ) {
        const uint64_t end = qMin( to, at + 4096 );
        log.snapshot( at, end, batch );
        for( const tw::LogRecord &rec : batch ) {
            if( filterLevel && rec.level != wantLevel ) continue;
            if( rec.text.find( needle ) != std::string::npos ) ++found;
        }
        at = end;
    }

    const bool tooFew  = found < minCount_;
    const bool tooMany = ( maxCount_ >= 0 && found > maxCount_ );
    if( tooFew || tooMany ) {
        qWarning() << "SAssertLogAction:" << found << "log record(s) contain"
                   << contains_
                   << ( filterLevel ? QString( " at level %1" ).arg( level_ )
                                    : QString() )
                   << "; expected at least" << minCount_
                   << ( maxCount_ >= 0 ? QString( " and at most %1" ).arg( maxCount_ )
                                       : QString() )
                   << "( window: log records" << (qulonglong) from << ".."
                   << (qulonglong) to << ", ring holds from"
                   << (qulonglong) log.firstSeq() << ")";
        return { false, nullptr };
    }

    qDebug() << "SAssertLogAction: OK —" << found << "record(s) contain"
             << contains_;
    return { true, nullptr };   // assertions are not undoable
}

void SAssertLogAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "contains", contains_ );
    elem.setAttribute( "minCount", QString::number( minCount_ ) );
    elem.setAttribute( "maxCount", QString::number( maxCount_ ) );
    if( !level_.isEmpty() ) elem.setAttribute( "level", level_ );
}

bool SAssertLogAction::readXml( const QDomElement &elem, int /*version*/ )
{
    // A missing `contains` is reported by apply(), not here: a readXml that
    // fails makes the action undeserializable, which breaks the round-trip
    // audit (it feeds every verb a DEFAULT instance through the trip).
    contains_ = elem.attribute( "contains", "" );
    bool ok1 = false, ok2 = false;
    minCount_ = elem.attribute( "minCount", "1" ).toInt( &ok1 );
    maxCount_ = elem.attribute( "maxCount", "-1" ).toInt( &ok2 );
    level_    = elem.attribute( "level", "" );
    if( !ok1 || !ok2 ) {
        qWarning() << "SAssertLogAction::readXml: invalid minCount/maxCount";
        return false;
    }
    return true;
}

static const bool s_reg_assert_log = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-log"),
        []{ return new SAssertLogAction; }
    ), true
);
