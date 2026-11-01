#include "app/shell/slanebadge.h"

#include <QCursor>
#include <QFont>
#include <QPalette>

#include "app/model/splacements.h"
#include "app/model/sproject.h"

SLaneBadge::SLaneBadge( const QString &objectName, QWidget *parent )
    : QToolButton( parent )
{
    setObjectName( objectName );
    setAutoRaise( true );
    setCursor( Qt::PointingHandCursor );
    setFocusPolicy( Qt::NoFocus );
    QFont f = font();
    f.setBold( true );
    setFont( f );
    // THE COLOUR COMES FROM THE PALETTE, never from a hex literal: the same
    // literal that reads as "lit" on the light theme is unreadable on the
    // dark one, and these widgets have to work on both. Highlight is the role
    // the platform theme already guarantees contrasts with the window.
    QPalette p = palette();
    p.setColor( QPalette::ButtonText, p.color( QPalette::Highlight ) );
    setPalette( p );
    // EXPLICITLY hidden, so isHidden() means what describe() says it means.
    // A widget that was merely never shown is hidden in a different sense --
    // one a headless run cannot tell from the state this sets -- and the gate
    // for "no flag, no badge" rests on this being the explicit kind.
    hide();
}

void SLaneBadge::apply_( const QString &text, const QString &tip )
{
    setText( text );
    setToolTip( tip );
    setVisible( lanes_ > 0 );
}

void SLaneBadge::clearState()
{
    lanes_ = 0;
    roots_.clear();
    specs_.clear();
    onScreen_ = true;
    apply_( QString(), QString() );
}

void SLaneBadge::refreshFrom( SProject *project, SObject *activeRoot,
                              const QString &masterLabel,
                              const QList<SObject *> &lanes,
                              const Words &words )
{
    lanes_ = 0;
    roots_.clear();
    specs_.clear();
    onScreen_ = true;

    if( !project ) { clearState(); return; }

    for( SObject *lane : lanes ) {
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

    if( lanes_ == 0 ) { apply_( QString(), QString() ); return; }

    // THE WORD CARRIES THE STATE; everything after the separator says WHERE.
    // The root is named whenever it is not the one on screen, because that is
    // the whole point -- and with the lanes spread over several roots a count
    // is the only honest short answer, so the tooltip carries the list.
    //
    // THE SEPARATOR IS A · WRITTEN AS AN ESCAPE, not as a raw byte in a
    // tr() string. QStringLiteral expands to a u"" literal, where · is
    // one UTF-16 code unit; the same two UTF-8 bytes spelled \xC2\xB7 there
    // would become TWO characters and paint as mojibake. Keeping it out of the
    // translatable strings also means no translator has to carry it.
    const QString sep = QStringLiteral( " · " );
    QString text = words.word;
    if( !onScreen_ && roots_.size() == 1 )
        text += sep + ( lanes_ == 1
                            ? roots_.first()
                            : tr( "%1 (%n lanes)", "", lanes_ )
                                  .arg( roots_.first() ) );
    else if( lanes_ > 1 )
        text += sep + tr( "%n lanes", "", lanes_ );

    const QString tip = words.what + QStringLiteral( "\n" )
                        + words.listLabel.arg(
                              specs_.join( QStringLiteral( " " ) ) )
                        + QStringLiteral( "\n" ) + words.click;
    apply_( text, tip );
}

QString SLaneBadge::specList() const
{
    return specs_.join( QStringLiteral( " " ) );
}

QString SLaneBadge::describeFields( const QString &prefix ) const
{
    return QStringLiteral( "%1Shown=%2|%1Text=%3|%1Lanes=%4|%1Roots=%5"
                           "|%1OnScreen=%6" )
        .arg( prefix )
        .arg( isHidden() ? 0 : 1 )
        .arg( text() )
        .arg( lanes_ )
        .arg( roots_.join( QStringLiteral( ";" ) ) )
        .arg( lanes_ > 0 && onScreen_ ? 1 : 0 );
}
