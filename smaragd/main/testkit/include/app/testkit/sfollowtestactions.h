#ifndef SFOLLOWTESTACTIONS_H
#define SFOLLOWTESTACTIONS_H

#include "app/actions/saction.h"
#include "tw/core/twtypes.h"
#include <QString>

// QBX-237: the two knobs a follow-the-playhead case needs that no other verb
// gives it. Neither is undoable: a playback tick and a clock are not edits.

// advance-locator -- ONE tick of the playback poll (SApplication::
// tkPumpLocatorTo): the position the audio thread would have stored, then
// pumpLocator(), so globalLocatorMoved AND locatorAdvanced fire exactly as
// they do under playback. `set-locator` is a SEEK and emits only the first,
// which is why it can never make the view follow. Deterministic: no device,
// no wall clock.
//
//   position = "0"   project FRAMES (>= 0)
//
// REJECTED while the transport is playing or recording, where the audio
// thread owns the locator and a scripted tick would race it.
class SAdvanceLocatorAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "advance-locator" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "position" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    offset_t position_ = 0;
};

// advance-follow-clock -- move the ACTIVE arranger's follow-hold clock forward
// (SMVActualView::tkAdvanceFollowClock), so a case can stand 4900 ms or
// 5100 ms after a manual scroll without waiting. Only the hold reads it.
//
//   ms = "0"   milliseconds (>= 0; negative REJECTED on read)
//
// REJECTED with no arranger.
class SAdvanceFollowClockAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "advance-follow-clock" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "ms" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    qint64 ms_ = 0;
};

#endif  // SFOLLOWTESTACTIONS_H
