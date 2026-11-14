#include "app/testkit/sassertsignalreceiversaction.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "app/shell/slivemonitor.h"
#include "app/shell/ssettings.h"
#include <QDebug>
#include <QDomElement>
#include <QHash>
#include <QMetaMethod>

namespace {

// QObject::receivers() is protected. A pointer to it, formed inside a class
// derived from QObject, is an ordinary `int (QObject::*)(const char *) const`
// and may be called on any QObject -- the access check is on forming the
// pointer, not on the call. This keeps the count out of SApplication's and
// SSettings' public API: it is a test's question, not the product's.
struct ReceiversPeek : QObject {
    static int count( const QObject *o, const char *signal )
    {
        int ( QObject::*fn )( const char * ) const = &ReceiversPeek::receivers;
        return ( o->*fn )( signal );
    }
};

// What `record` remembered, for a later `since`. Per run, keyed by the case.
QHash<QString, int> &recorded_()
{
    static QHash<QString, int> h;
    return h;
}

QObject *senderNamed_( const QString &name )
{
    if( name == QLatin1String( "app" ) ) return &SApplication::app();
    if( name == QLatin1String( "settings" ) ) return &SSettings::instance();
    if( name == QLatin1String( "liveMonitor" ) )
        return SApplication::app().liveMonitor();
    return nullptr;
}

}  // namespace

// One signal, counted. Empty on success; otherwise why it could not be.
static QString countOne_( const QString &entry, int &n )
{
    const int dot = entry.indexOf( QLatin1Char( '.' ) );
    if( dot <= 0 || dot == entry.size() - 1 )
        return QStringLiteral( "'%1' is not <sender>.<signal>" ).arg( entry );
    const QString senderName = entry.left( dot );
    const QString signalName = entry.mid( dot + 1 );
    QObject *s = senderNamed_( senderName );
    if( !s ) return QStringLiteral( "no sender '%1'" ).arg( senderName );

    // The signal by NAME. An overloaded one is refused: which overload a case
    // meant is not something to guess.
    const QMetaObject *mo = s->metaObject();
    QByteArray sig;
    int found = 0;
    for( int i = 0; i < mo->methodCount(); ++i ) {
        const QMetaMethod m = mo->method( i );
        if( m.methodType() != QMetaMethod::Signal ) continue;
        if( QString::fromLatin1( m.name() ) != signalName ) continue;
        sig = m.methodSignature();
        ++found;
    }
    if( found != 1 )
        return QStringLiteral( "'%1' %2" ).arg( entry,
            found ? QStringLiteral( "is overloaded" )
                  : QStringLiteral( "is not a signal of that sender" ) );
    // The SIGNAL() macro's spelling: the code '2' before the signature.
    const QByteArray spelled = QByteArray( "2" ) + sig;
    n = ReceiversPeek::count( s, spelled.constData() );
    return QString();
}

SApplyResult SAssertSignalReceiversAction::apply( SProject * /*project*/ )
{
    if( signals_.isEmpty() ) {
        qWarning() << "assert-signal-receivers: no signals given";
        return { false, nullptr };
    }
    if( !hasCount_ && record_.isEmpty() && since_.isEmpty() ) {
        qWarning() << "assert-signal-receivers: give count, record, or since+delta";
        return { false, nullptr };
    }
    if( since_.isEmpty() != !hasDelta_ ) {
        qWarning() << "assert-signal-receivers: since and delta go together";
        return { false, nullptr };
    }

    // EVERY signal is counted before anything is reported, in this one
    // action: a retired view's deferred delete can be delivered by the
    // runner's processEvents() right after it, so a second action would
    // measure a different state (QBX-301).
    QList<int> counts;
    for( const QString &entry : signals_ ) {
        int n = 0;
        const QString err = countOne_( entry, n );
        if( !err.isEmpty() ) {
            qWarning().noquote() << "assert-signal-receivers:" << err;
            return { false, nullptr };
        }
        counts << n;
    }

    bool ok = true;
    for( int i = 0; i < signals_.size(); ++i ) {
        const QString &what = signals_.at( i );
        const int n = counts.at( i );
        qInfo().noquote() << QStringLiteral( "assert-signal-receivers: %1 reaches %2 listener(s)" )
                                 .arg( what ).arg( n );
        if( hasCount_ && n != count_ ) {
            qWarning().noquote() << QStringLiteral(
                "assert-signal-receivers FAILED: %1 reaches %2 listener(s), expected %3" )
                .arg( what ).arg( n ).arg( count_ );
            ok = false;
        }
        const QString sinceKey = since_ + QLatin1Char( '|' ) + what;
        if( !since_.isEmpty() ) {
            if( !recorded_().contains( sinceKey ) ) {
                qWarning().noquote() << QStringLiteral(
                    "assert-signal-receivers FAILED: nothing recorded as '%1' for %2" )
                    .arg( since_, what );
                ok = false;
            } else if( n - recorded_().value( sinceKey ) != delta_ ) {
                qWarning().noquote() << QStringLiteral(
                    "assert-signal-receivers FAILED: %1 reaches %2 listener(s), %3 "
                    "since '%4' (%5), expected a delta of %6" )
                    .arg( what ).arg( n ).arg( n - recorded_().value( sinceKey ) )
                    .arg( since_ ).arg( recorded_().value( sinceKey ) ).arg( delta_ );
                ok = false;
            }
        }
        if( !record_.isEmpty() )
            recorded_().insert( record_ + QLatin1Char( '|' ) + what, n );
    }
    return { ok, nullptr };
}

void SAssertSignalReceiversAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "signals", signals_.join( QLatin1Char( ',' ) ) );
    if( hasCount_ ) elem.setAttribute( "count", count_ );
    if( !record_.isEmpty() ) elem.setAttribute( "record", record_ );
    if( !since_.isEmpty() ) elem.setAttribute( "since", since_ );
    if( hasDelta_ ) elem.setAttribute( "delta", delta_ );
}

bool SAssertSignalReceiversAction::readXml( const QDomElement &elem, int /*version*/ )
{
    signals_.clear();
    for( const QString &e : elem.attribute( "signals" ).split( QLatin1Char( ',' ),
                                                               Qt::SkipEmptyParts ) )
        signals_ << e.trimmed();
    record_ = elem.attribute( "record" );
    since_  = elem.attribute( "since" );
    bool ok = true;
    hasCount_ = elem.hasAttribute( "count" );
    if( hasCount_ ) count_ = elem.attribute( "count" ).toInt( &ok );
    if( !ok || count_ < 0 ) {
        qWarning() << "assert-signal-receivers: count must be an integer >= 0, got"
                   << elem.attribute( "count" );
        return false;
    }
    hasDelta_ = elem.hasAttribute( "delta" );
    if( hasDelta_ ) delta_ = elem.attribute( "delta" ).toInt( &ok );
    if( !ok ) {
        qWarning() << "assert-signal-receivers: delta must be an integer, got"
                   << elem.attribute( "delta" );
        return false;
    }
    return true;
}

static const bool s_reg_signalreceivers = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-signal-receivers" ),
        [] { return new SAssertSignalReceiversAction; } ),
    true );
