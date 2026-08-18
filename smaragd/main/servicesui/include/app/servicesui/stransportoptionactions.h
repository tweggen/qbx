#ifndef STRANSPORTOPTIONACTIONS_H
#define STRANSPORTOPTIONACTIONS_H

#include "app/actions/saction.h"

/**
 * `set-count-in` and `set-pre-roll` (proposal 21 L5, design section 6).
 *
 * Both write a per-USER option (`SOpt::CountInBars` / `SOpt::PreRollBars`), not
 * a project property, and both are therefore NOT UNDOABLE — the same class as
 * `set-option` and for the same reason: how many bars this person likes before
 * a take is a preference, and putting a preference on the arrangement's undo
 * stack would make Ctrl-Z mean two different things.
 *
 * They exist as NAMED verbs rather than as two spellings of `set-option`
 * because they are the two knobs proposal 21 §6 lists by name, because a named
 * verb can validate its range (a "count-in" of 40 bars is a typo, not a
 * choice), and because `set-option` lives in the TESTKIT while these are
 * production behaviour a UI will bind to.
 *
 * They live in `app/servicesui` for the same reason the Options dialog does:
 * this is the module that owns the per-user option table, and it is the only
 * one that may include both `SOpt` and `SSettings`.
 *
 *   <set-count-in bars="1"/>
 *   <set-pre-roll  bars="2"/>
 */
class STransportBarsAction : public SAction
{
public:
    /// The greatest number of bars either knob accepts. Eight bars at 60 BPM
    /// is half a minute of waiting before every take; past that it is a typo.
    static constexpr int kMaxBars = 8;

    explicit STransportBarsAction( bool preRoll ) : preRoll_( preRoll ) {}

    QString name() const override;
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    bool preRoll_ = false;
    int  bars_    = 0;
};

#endif // STRANSPORTOPTIONACTIONS_H
