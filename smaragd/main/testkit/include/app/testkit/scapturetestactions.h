#ifndef _SCAPTURETESTACTIONS_H_
#define _SCAPTURETESTACTIONS_H_

#include "app/actions/saction.h"
#include <QString>

/**
 * QBX-283: the test verbs of an asset's capture hold (objects/cut, SCut).
 *
 * `set-capture-gate assetName= ready=` closes (ready="false") or opens the
 * asset body's capture build gate: a build on another thread waits there,
 * after the live-hold before-check and before any content is read, so a case
 * can land an arm INSIDE a capture build (the after-check's race). With
 * holdMs > 0 the build waits at most that long and then runs on its own (a
 * build the arm's drain can wait out).
 *
 * `assert-capture-hold assetName= held= heldFor=` checks whether the body's
 * capture is held for a live-owned track (held "1"/"0") and for which one
 * (heldFor, the track's spec, compared exactly; "" when not held).
 */
class SSetCaptureGateAction : public SAction
{
public:
    SApplyResult apply( SProject *project ) override;
    QString name() const override { return QStringLiteral( "set-capture-gate" ); }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "assetName" ), QStringLiteral( "ready" ),
               QStringLiteral( "holdMs" ) }; }

private:
    QString assetName_;
    bool    ready_ = true;
    int     holdMs_ = 0;
};

class SAssertCaptureHoldAction : public SAction
{
public:
    SApplyResult apply( SProject *project ) override;
    QString name() const override { return QStringLiteral( "assert-capture-hold" ); }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "assetName" ), QStringLiteral( "held" ),
               QStringLiteral( "heldFor" ) }; }

private:
    QString assetName_;
    QString held_;
    QString heldFor_;
    bool    checkHeldFor_ = false;
};

#endif
