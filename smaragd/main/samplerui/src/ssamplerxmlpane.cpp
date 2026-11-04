#include "app/samplerui/ssamplerxmlpane.h"

#include "app/actions/saction.h"
#include "app/actions/sactionregistry.h"
#include "app/samplerui/ssamplereditor.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringListModel>
#include <QTextCursor>
#include <QVBoxLayout>

#include <memory>

namespace {

bool isNameChar( QChar c )
{
    return c.isLetterOrNumber() || c == QLatin1Char( '-' ) || c == QLatin1Char( '_' );
}

}  // namespace

SSamplerXmlPane::SSamplerXmlPane( SSamplerEditor *editor, QWidget *parent )
    : QWidget( parent ), editor_( editor )
{
    auto *lay = new QVBoxLayout( this );
    lay->setContentsMargins( 0, 0, 0, 0 );
    lay->setSpacing( 4 );
    edit_ = new QPlainTextEdit( this );
    edit_->setObjectName( QStringLiteral( "xmlEdit" ) );
    edit_->setPlaceholderText( tr( "<set-zone zone=\"1\" cutoff=\"2000\"/>   (track and slot are implied)" ) );
    edit_->setTabChangesFocus( true );
    edit_->setFixedHeight( 72 );
    QFont mono = edit_->font();
    mono.setStyleHint( QFont::Monospace );
    mono.setFamily( QStringLiteral( "monospace" ) );
    edit_->setFont( mono );
    lay->addWidget( edit_ );

    auto *row = new QHBoxLayout;
    row->addStretch( 1 );
    apply_ = new QPushButton( tr( "Apply" ), this );
    apply_->setObjectName( QStringLiteral( "xmlApply" ) );
    apply_->setToolTip( tr( "Apply the verb as one undoable step" ) );
    row->addWidget( apply_ );
    lay->addLayout( row );

    model_     = new QStringListModel( this );
    completer_ = new QCompleter( model_, this );
    completer_->setWidget( edit_ );
    completer_->setCompletionMode( QCompleter::PopupCompletion );
    completer_->setCaseSensitivity( Qt::CaseSensitive );
    connect( completer_, QOverload<const QString &>::of( &QCompleter::activated ), this,
             &SSamplerXmlPane::insertCompletion );
    connect( edit_, &QPlainTextEdit::textChanged, this, &SSamplerXmlPane::onTextChanged );
    connect( apply_, &QPushButton::clicked, this, [this]() { apply(); } );
}

QStringList SSamplerXmlPane::verbs()
{
    return { QStringLiteral( "add-zone" ),          QStringLiteral( "remove-zone" ),
             QStringLiteral( "add-layer" ),         QStringLiteral( "remove-layer" ),
             QStringLiteral( "set-zone" ),          QStringLiteral( "set-layer" ),
             QStringLiteral( "set-sampler-program" ), QStringLiteral( "select-zones" ),
             QStringLiteral( "set-sampler-view" ),  QStringLiteral( "set-pad-key" ),
             QStringLiteral( "set-sampler-record-input" ),
             QStringLiteral( "place-sampler-recording" ) };
}

QStringList SSamplerXmlPane::completionsAt( const QString &text, int cursor, QString *prefix )
{
    if( prefix ) prefix->clear();
    if( cursor < 0 || cursor > text.size() ) cursor = text.size();
    const QString before = text.left( cursor );
    const int lt = before.lastIndexOf( QLatin1Char( '<' ) );
    if( lt < 0 || before.indexOf( QLatin1Char( '>' ), lt ) >= 0 ) return {};
    const QString tag = before.mid( lt + 1 );

    // The name being typed: the run of name characters right before the cursor.
    int start = tag.size();
    while( start > 0 && isNameChar( tag.at( start - 1 ) ) ) --start;
    const QString typed = tag.mid( start );

    QStringList out;
    if( start == 0 ) {
        // Right after `<`: a verb.
        for( const QString &v : verbs() )
            if( v.startsWith( typed ) ) out << v;
    } else {
        // After a blank inside the tag, and not inside a quoted value.
        if( !tag.at( start - 1 ).isSpace() ) return {};
        if( tag.count( QLatin1Char( '"' ) ) % 2 != 0 ) return {};
        int vEnd = 0;
        while( vEnd < tag.size() && isNameChar( tag.at( vEnd ) ) ) ++vEnd;
        const QString verb = tag.left( vEnd );
        if( !verbs().contains( verb ) ) return {};
        std::unique_ptr<SAction> act( SActionRegistry::instance().create( verb ) );
        if( !act ) return {};
        // Attributes already written in the tag are not offered again.
        QStringList written;
        {
            const QString rest = tag.mid( vEnd );
            int i = 0;
            while( i < rest.size() ) {
                if( isNameChar( rest.at( i ) ) ) {
                    int j = i;
                    while( j < rest.size() && isNameChar( rest.at( j ) ) ) ++j;
                    int k = j;
                    while( k < rest.size() && rest.at( k ).isSpace() ) ++k;
                    if( k < rest.size() && rest.at( k ) == QLatin1Char( '=' ) )
                        written << rest.mid( i, j - i );
                    i = j;
                } else if( rest.at( i ) == QLatin1Char( '"' ) ) {
                    const int close = rest.indexOf( QLatin1Char( '"' ), i + 1 );
                    i = close < 0 ? rest.size() : close + 1;
                } else {
                    ++i;
                }
            }
        }
        for( const QString &a : act->knownAttributes() ) {
            // The address is implied here, and `gesture` is a script's
            // spelling of a drag (QBX-206), not something to type.
            if( a == QLatin1String( "trackPath" ) || a == QLatin1String( "slotIndex" ) ||
                a == QLatin1String( "gesture" ) )
                continue;
            if( written.contains( a ) || !a.startsWith( typed ) ) continue;
            if( !out.contains( a ) ) out << a;
        }
    }
    if( prefix ) *prefix = typed;
    return out;
}

void SSamplerXmlPane::onTextChanged()
{
    if( inserting_ ) return;
    const QTextCursor c = edit_->textCursor();
    QString prefix;
    const QStringList list = completionsAt( edit_->toPlainText(), c.position(), &prefix );
    prefix_ = prefix;
    model_->setStringList( list );
    // Only a VISIBLE pane pops the list up: a --test-case run never shows the
    // window, and a popup is a top-level window of its own.
    if( !list.isEmpty() && edit_->isVisible() ) {
        completer_->setCompletionPrefix( QString() );
        QRect r = edit_->cursorRect();
        r.setWidth( completer_->popup()->sizeHintForColumn( 0 ) + 24 );
        completer_->complete( r );
    } else if( completer_->popup() ) {
        completer_->popup()->hide();
    }
}

void SSamplerXmlPane::insertCompletion( const QString &completion )
{
    QTextCursor c = edit_->textCursor();
    inserting_ = true;
    for( int i = 0; i < prefix_.size(); ++i ) c.deletePreviousChar();
    // An attribute gets its `=""` with the cursor between the quotes; a verb
    // a blank, ready for the first attribute.
    const bool isVerb = verbs().contains( completion );
    if( isVerb ) {
        c.insertText( completion + QLatin1Char( ' ' ) );
    } else {
        c.insertText( completion + QStringLiteral( "=\"\"" ) );
        c.movePosition( QTextCursor::Left );
    }
    edit_->setTextCursor( c );
    inserting_ = false;
    prefix_.clear();
    model_->setStringList( {} );
}

void SSamplerXmlPane::typeText( const QString &text, int cursor )
{
    // The text arrives with signals blocked, the cursor is put where the case
    // says, and THEN the same handler the typing runs is called: a setPlainText
    // with signals on would complete at the end of the text, before the cursor
    // moved.
    edit_->blockSignals( true );
    edit_->setPlainText( text );
    QTextCursor c = edit_->textCursor();
    c.setPosition( ( cursor < 0 || cursor > text.size() ) ? text.size() : cursor );
    edit_->setTextCursor( c );
    edit_->blockSignals( false );
    onTextChanged();
}

QStringList SSamplerXmlPane::offered() const { return model_->stringList(); }

bool SSamplerXmlPane::acceptCompletion( const QString &choice )
{
    if( !model_->stringList().contains( choice ) ) return false;
    // Through the completer's own signal, so the activated -> insert wiring is
    // on the path.
    emit completer_->activated( choice );
    return true;
}

bool SSamplerXmlPane::apply()
{
    QString why;
    const QString text = edit_->toPlainText().trimmed();
    if( text.isEmpty() ) {
        status_ = tr( "Nothing to apply." );
        editor_->setStatus( status_ );
        return false;
    }
    const bool ok = editor_->submitVerbXml( text, 0, &why );
    status_ = ok ? tr( "Applied." ) : why;
    editor_->setStatus( status_ );
    return ok;
}

QString SSamplerXmlPane::text() const { return edit_->toPlainText(); }
