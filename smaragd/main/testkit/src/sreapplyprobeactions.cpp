#include "app/testkit/sreapplyprobeactions.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"

namespace {
int s_staleUndos = 0;
int s_undos      = 0;

// The inverse: not a registered verb, only ever made by probe-reapply.
class SProbeReapplyInverse : public SAction
{
public:
    SProbeReapplyInverse( SProbeReapplyAction *owner, int stamp )
        : owner_( owner ), stamp_( stamp ) {}
    QString name() const override { return QStringLiteral( "probe-reapply-inverse" ); }
    SApplyResult apply( SProject * ) override
    {
        // owner_ is the forward, owned by the same undo command as this.
        const bool stale = owner_->applications() != stamp_;
        SProbeReapplyAction::noteUndo( stale );
        if( stale )
            qWarning().noquote() << "probe-reapply: the undo ran the inverse of application"
                                 << stamp_ << "but the forward has been applied"
                                 << owner_->applications() << "time(s)";
        return { true, nullptr };
    }
    void writeXml( QDomElement & ) const override {}
    bool readXml( const QDomElement &, int ) override { return false; }

private:
    SProbeReapplyAction *owner_;
    int                  stamp_;
};
}  // namespace

int SProbeReapplyAction::staleUndos() { return s_staleUndos; }
int SProbeReapplyAction::undos() { return s_undos; }
void SProbeReapplyAction::noteUndo( bool stale )
{
    ++s_undos;
    if( stale ) ++s_staleUndos;
}

SApplyResult SProbeReapplyAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    ++applications_;
    return { true, new SProbeReapplyInverse( this, applications_ ) };
}

void SProbeReapplyAction::writeXml( QDomElement & ) const {}

bool SProbeReapplyAction::readXml( const QDomElement &, int ) { return true; }

SApplyResult SAssertReapplyProbeAction::apply( SProject * )
{
    const int stale = SProbeReapplyAction::staleUndos();
    const int undos = SProbeReapplyAction::undos();
    const bool ok = stale == stale_ && ( undos_ < 0 || undos == undos_ );
    if( !ok )
        qWarning().noquote() << "assert-reapply-probe FAILED: want stale=" << stale_
                             << "undos=" << undos_ << "; got stale=" << stale
                             << "undos=" << undos;
    else
        qInfo().noquote() << "assert-reapply-probe OK: stale=" << stale << "undos=" << undos;
    return { ok, nullptr };
}

void SAssertReapplyProbeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "stale", stale_ );
    if( undos_ >= 0 ) elem.setAttribute( "undos", undos_ );
}

bool SAssertReapplyProbeAction::readXml( const QDomElement &elem, int )
{
    bool ok = false;
    stale_ = elem.attribute( "stale", "0" ).toInt( &ok );
    if( !ok ) return false;
    undos_ = elem.hasAttribute( "undos" ) ? elem.attribute( "undos" ).toInt( &ok ) : -1;
    return ok;
}

static const bool s_reg_probe_reapply = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "probe-reapply" ),
        []{ return new SProbeReapplyAction; } ), true );

static const bool s_reg_assert_reapply_probe = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-reapply-probe" ),
        []{ return new SAssertReapplyProbeAction; } ), true );
