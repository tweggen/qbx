#ifndef SASSERTSENDSACTION_H
#define SASSERTSENDSACTION_H

#include "app/actions/saction.h"

/**
 * `assert-sends` — proposal 47 M0.
 *
 * Reads a lane's send TAPS back through the model. **In M0 this is the ONLY
 * thing that can bite**, and that is the same shape the missing-sample
 * placeholder had: until M1 wires a send bus, a tap that is present and a tap
 * that was silently dropped are BOTH inaudible, so no audio assertion anywhere
 * can separate them.
 *
 * `count` is the number of taps on the lane. `dest` selects one tap and turns
 * on the per-tap assertions (`level`, `pre`, `enabled`); `absent="true"`
 * asserts that `dest` is NOT among them, which is what an undo has to restore.
 */
class SAssertSendsAction : public SAction {
public:
    SAssertSendsAction() = default;

    QString name() const override { return QStringLiteral( "assert-sends" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "track" ), QStringLiteral( "count" ),
               QStringLiteral( "dest" ), QStringLiteral( "level" ),
               QStringLiteral( "pre" ), QStringLiteral( "enabled" ),
               QStringLiteral( "absent" ), QStringLiteral( "tolerance" ),
               QStringLiteral( "arrangement" ) }; }

private:
    QString trackPath_;
    QString arrangement_;
    QString dest_;
    QString pre_;
    QString enabled_;
    QString absent_;
    int     count_ = -1;
    double  level_ = 0.0;
    bool    hasLevel_ = false;
    double  tolerance_ = 1e-9;
};

#endif
