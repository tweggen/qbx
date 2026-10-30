#include "app/shell/ssoloindicator.h"

#include <QCursor>
#include <QFont>
#include <QPalette>

#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/ssolorules.h"

SSoloIndicator::SSoloIndicator( QWidget *parent )
    : QToolButton( parent )
{
    setObjectName( QStringLiteral( "soloIndicator" ) );
    setAutoRaise( true );
    setCursor( Qt::PointingHandCursor );
    setFocusPolicy( Qt::NoFocus );
    QFont f = font();
    f.setBold( true );
    setFont( f );
    // THE COLOUR COMES FROM THE PALETTE, never from a hex literal: the same
    // literal that reads as "lit" on the light theme is unreadable on the
    // dark one, and this widget has to work on both. Highlight is the role
    // the platform theme already guarantees contrasts with the window.
    QPalette p = palette();
    p.setColor( QPalette::ButtonText, p.color( QPalette::Highlight ) );
    setPalette( p );
    // EXPLICITLY hidden, so isHidden() means what describe() says it means.
    // A widget that was merely never shown is hidden in a different sense --
    // one a headless run cannot tell from the state this sets -- and the gate
    // for "no solo, no badge" rests on this being the explicit kind.
    hide();
}

void SSoloIndicator::apply_( const QString &text, const QString &tip )
{
    setText( text );
    setToolTip( tip );
    setVisible( lanes_ > 0 );
}

void SSoloIndicator::refresh( SProject *project, SObject *activeRoot,
                              const QString &masterLabel )
{
    lanes_ = 0;
    roots_.clear();
    specs_.clear();
    onScreen_ = true;
    scope_ = QStringLiteral( "global" );

    if( !project ) {
        apply_( QString(), QString() );
        return;
    }
    scope_ = project->soloScope() == SProject::SoloScope::Global
                 ? QStringLiteral( "global" )
                 : QStringLiteral( "arrangement" );

    const QList<SObject *> soloed = ssolo::soloedLanesInProject( project );
    for( SObject *lane : soloed ) {
        if( !lane ) continue;
        // specOf() and rootHolding() come out of ONE walk in one order by
        // construction (see splacements.h), so the spelling and the root
        // cannot describe different places -- which is exactly what deriving
        // the root by re-resolving the qualifier would risk.
        const QString spec = splacements::specOf( project, lane );
        SObject *root = splacements::rootHolding( project, lane );
        if( spec.isEmpty() || !root ) continue;   // left the project
        ++lanes_;
        specs_ << spec;
        const int colon = spec.indexOf( QLatin1Char( ':' ) );
        const QString label = colon > 0 ? spec.left( colon ) : masterLabel;
        if( !roots_.contains( label ) ) roots_ << label;
        if( root != activeRoot ) onScreen_ = false;
    }

    if( lanes_ == 0 ) {
        apply_( QString(), QString() );
        return;
    }

    // SOLO is the word; everything after the separator says WHERE. The root
    // is named whenever it is not the one on screen, because that is the
    // whole point -- and with the solos spread over several roots a count is
    // the only honest short answer, so the tooltip carries the list.
    //
    // THE SEPARATOR IS A \u00B7 WRITTEN AS AN ESCAPE, not as a raw byte in a
    // tr() string. QStringLiteral expands to a u"" literal, where \u00B7 is
    // one UTF-16 code unit; the same two UTF-8 bytes spelled \xC2\xB7 there
    // would become TWO characters and paint as mojibake. Keeping it out of the
    // translatable strings also means no translator has to carry it.
    const QString sep = QStringLiteral( " \u00B7 " );
    QString text = QStringLiteral( "SOLO" );
    if( !onScreen_ && roots_.size() == 1 )
        text += sep + ( lanes_ == 1
                            ? roots_.first()
                            : tr( "%1 (%n lanes)", "", lanes_ )
                                  .arg( roots_.first() ) );
    else if( lanes_ > 1 )
        text += sep + tr( "%n lanes", "", lanes_ );

    // THE TOOLTIP SAYS WHAT THE SCOPE MAKES TRUE, and the two sentences are
    // different claims. In `global` a solo anywhere is why the rest of the
    // project is quiet; in `arrangement` it is not, and a badge that implied
    // otherwise would be the lie item 6 of QBX-257 forbids.
    const QString what =
        scope_ == QLatin1String( "global" )
            ? tr( "Solo is in force: only the soloed lane(s) play, in every "
                  "arrangement." )
            : tr( "Solo is in force inside its own arrangement: it changes "
                  "what that arrangement's placements play, not the rest of "
                  "the project." );
    const QString tip = what + QStringLiteral( "\n" )
                        + tr( "Soloed: %1" ).arg( specs_.join( QStringLiteral( " " ) ) )
                        + QStringLiteral( "\n" )
                        + tr( "Click to release every solo (one undo step)." );
    apply_( text, tip );
}

QString SSoloIndicator::describe() const
{
    return QStringLiteral( "soloShown=%1|soloText=%2|soloLanes=%3|soloRoots=%4"
                           "|soloOnScreen=%5|soloScope=%6|soloList=%7" )
        .arg( isHidden() ? 0 : 1 )
        .arg( text() )
        .arg( lanes_ )
        .arg( roots_.join( QStringLiteral( ";" ) ) )
        .arg( lanes_ > 0 && onScreen_ ? 1 : 0 )
        .arg( scope_ )
        .arg( specs_.join( QStringLiteral( " " ) ) );
}
