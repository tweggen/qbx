#ifndef STOGGLESETTINGACTION_H
#define STOGGLESETTINGACTION_H

#include "app/actions/saction.h"
#include <QDomElement>

// Base for simple boolean app-setting actions (snap-to-grid, grid, metronome,
// cycle, ...). Each concrete subclass binds a single boolean setting; the same
// subclass provides three operations selected by Op:
//
//   Toggle  -> flip the setting
//   Enable  -> force it on
//   Disable -> force it off
//
// Registered under three verbs: "<base>-toggle", "<base>-enable",
// "<base>-disable". Non-undoable (these are transient UI/transport settings,
// like transport playback).
class SToggleSettingAction : public SAction {
public:
    enum Op { Toggle = 0, Enable = 1, Disable = 2 };

    explicit SToggleSettingAction( Op op = Toggle ) : op_( op ) {}

    QString name() const override {
        switch( op_ ) {
            case Enable:  return baseName() + QStringLiteral("-enable");
            case Disable: return baseName() + QStringLiteral("-disable");
            case Toggle:
            default:      return baseName() + QStringLiteral("-toggle");
        }
    }

    SApplyResult apply( SProject *project ) override {
        if( !project ) return { false, nullptr };
        bool target = ( op_ == Toggle ) ? !getState( project )
                                        : ( op_ == Enable );
        setState( project, target );
        return { true, nullptr };   // non-undoable (transient view/transport setting)
    }

    void writeXml( QDomElement &elem ) const override {
        elem.setAttribute( "op", (int) op_ );
    }
    bool readXml( const QDomElement &elem, int /*version*/ ) override {
        op_ = (Op) elem.attribute( "op", "0" ).toInt();
        return true;
    }

protected:
    // Stable verb stem, e.g. "snap-to-grid". name() appends the operation.
    virtual QString baseName() const = 0;
    // The bound boolean project property.
    virtual bool getState( SProject *project ) const = 0;
    virtual void setState( SProject *project, bool ) = 0;

    Op op_;
};

#endif // STOGGLESETTINGACTION_H
