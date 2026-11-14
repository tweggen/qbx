#ifndef SASSERTSIGNALRECEIVERSACTION_H
#define SASSERTSIGNALRECEIVERSACTION_H

#include "app/actions/saction.h"
#include <QString>
#include <QStringList>

// assert-signal-receivers -- how many slots a signal of a LONG-LIVED sender
// reaches right now (QBX-301, shell CONTRACT inv. 72). QObject::receivers(),
// a count: no receiver is ever dereferenced, so the verb answers honestly
// while a retired view whose model is freed is still connected -- which is
// exactly the state it exists to see.
//
//   signals = "app.globalLocatorMoved,settings.changed,..."
//            a comma list of <sender>.<signal>. Senders: app
//            (SApplication::app()), settings (SSettings::instance()),
//            liveMonitor (SApplication::app().liveMonitor()). A signal is
//            named without arguments; an overloaded name is REJECTED rather
//            than guessed
//   count  = "N"     each count must be exactly N
//   record = "key"   remember each count under `key` (always passes)
//   since  = "key"   with delta: each count minus what `key` recorded for
//   delta  = "N"     the same signal must be exactly N
//
// ONE action counts every listed signal, and that is the reason for the list:
// a view retired inside a queued handler (open-project, close-project) is
// deleted by the runner's processEvents() after the NEXT action, so only one
// action sees the state between the model's death and the view's (QBX-301).
// An absolute count pins every OTHER listener of an app-wide signal too, so a
// case about one view's connections records before and asserts a delta
// after. Each count is logged as "<sender>.<signal> reaches N listener(s)" at
// info.
// Not undoable, like every assert-*.
class SAssertSignalReceiversAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "assert-signal-receivers" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "signals" ), QStringLiteral( "count" ), QStringLiteral( "record" ),
               QStringLiteral( "since" ), QStringLiteral( "delta" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QStringList signals_;
    QString record_;
    QString since_;
    bool hasCount_ = false;
    int count_ = 0;
    bool hasDelta_ = false;
    int delta_ = 0;
};

#endif  // SASSERTSIGNALRECEIVERSACTION_H
