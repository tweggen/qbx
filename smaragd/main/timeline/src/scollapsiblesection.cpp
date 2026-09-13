#include "app/timeline/scollapsiblesection.h"

#include <QToolButton>
#include <QVBoxLayout>

SCollapsibleSection::SCollapsibleSection( const QString &id,
                                          const QString &title,
                                          QWidget *parent )
    : QWidget( parent ), id_( id )
{
    QVBoxLayout *outer = new QVBoxLayout( this );
    outer->setContentsMargins( 0, 0, 0, 0 );
    outer->setSpacing( 0 );

    header_ = new QToolButton( this );
    header_->setObjectName( QStringLiteral( "sectionHeader_" ) + id );
    header_->setText( title );
    header_->setToolButtonStyle( Qt::ToolButtonTextBesideIcon );
    header_->setAutoRaise( true );
    header_->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Fixed );
    outer->addWidget( header_, 0 );

    // NO MARGINS: a widget moved into a section keeps the width and the left
    // edge it had before (the request's "without indenting today's content").
    content_ = new QWidget( this );
    contentLayout_ = new QVBoxLayout( content_ );
    contentLayout_->setContentsMargins( 0, 0, 0, 0 );
    contentLayout_->setSpacing( 0 );
    outer->addWidget( content_, 1 );

    connect( header_, &QToolButton::clicked, this,
             [this] { emit toggleRequested( !expanded_ ); } );

    setExpanded( true );
}

void SCollapsibleSection::setExpanded( bool expanded )
{
    expanded_ = expanded;
    header_->setArrowType( expanded ? Qt::DownArrow : Qt::RightArrow );
    content_->setVisible( expanded );
    // A collapsed section is its header row and nothing more: it must not keep
    // claiming vertical stretch it has no content to fill.
    setSizePolicy( QSizePolicy::Preferred,
                   expanded ? QSizePolicy::Preferred : QSizePolicy::Maximum );
}
