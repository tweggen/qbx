#include "app/servicesui/spurgeunusedclipsdialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "tw/core/twlog.h"

#include "app/shell/sapplication.h"

SPurgeUnusedClipsDialog::SPurgeUnusedClipsDialog( QWidget *parent,
                                                  const QStringList &rows )
    : QDialog( parent )
{
    setWindowTitle( tr( "Purge unused clips" ) );
    setModal( true );
    setObjectName( QStringLiteral( "purgeUnusedClipsDialog" ) );

    QVBoxLayout *outer = new QVBoxLayout( this );

    QLabel *summary = new QLabel( summaryText(), this );
    summary->setObjectName( QStringLiteral( "purgeUnusedClipsSummary" ) );
    summary->setWordWrap( true );
    summary->setMinimumWidth( 380 );
    outer->addWidget( summary );

    // ONE BOX PER ROW, ALL OFF, never remembered (the header's rule 1). In a
    // scroll area: a project can carry many orphans, and an unbounded column
    // would push the buttons off the screen.
    QWidget *list = new QWidget( this );
    QVBoxLayout *listLayout = new QVBoxLayout( list );
    listLayout->setContentsMargins( 0, 0, 0, 0 );
    for( int i = 0; i < rows.size(); ++i ) {
        QCheckBox *b = new QCheckBox( rows.at( i ), list );
        b->setObjectName( QStringLiteral( "purgeUnusedClipsRow%1" ).arg( i ) );
        b->setChecked( false );
        connect( b, &QCheckBox::toggled, this,
                 &SPurgeUnusedClipsDialog::refreshOk_ );
        listLayout->addWidget( b );
        boxes_ << b;
    }
    listLayout->addStretch( 1 );
    QScrollArea *scroll = new QScrollArea( this );
    scroll->setWidgetResizable( true );
    scroll->setWidget( list );
    outer->addWidget( scroll, 1 );

    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
    ok_ = buttons->button( QDialogButtonBox::Ok );
    if( ok_ ) {
        ok_->setText( tr( "Purge" ) );
        ok_->setObjectName( QStringLiteral( "purgeUnusedClipsOk" ) );
        ok_->setAutoDefault( false );
        ok_->setDefault( false );
    }
    // The SAFE answer is the default one (rule 2): this cannot be undone.
    if( QPushButton *cancel = buttons->button( QDialogButtonBox::Cancel ) )
        cancel->setDefault( true );
    connect( buttons, &QDialogButtonBox::accepted, this, &QDialog::accept );
    connect( buttons, &QDialogButtonBox::rejected, this, &QDialog::reject );
    outer->addWidget( buttons );

    refreshOk_();
}

QString SPurgeUnusedClipsDialog::summaryText() const
{
    return tr( "These clips were loaded with the project, and nothing in it "
               "uses them. Each save keeps them until you purge them.\n"
               "Purging a clip also removes what only that clip shows: its "
               "sample's entry in the project, or a placeholder for a missing "
               "file. Files on disk are not touched.\n"
               "This cannot be undone." );
}

QList<int> SPurgeUnusedClipsDialog::chosenRows() const
{
    QList<int> out;
    for( int i = 0; i < boxes_.size(); ++i )
        if( boxes_.at( i )->isChecked() ) out << i;
    return out;
}

void SPurgeUnusedClipsDialog::refreshOk_()
{
    if( ok_ ) ok_->setEnabled( !chosenRows().isEmpty() );
}

bool SPurgeUnusedClipsDialog::ask( QWidget *parent, const QStringList &rows,
                                   QList<int> *out )
{
    if( out ) out->clear();

    // HEADLESS FIRST, and it answers NO (convention: a modal confirmation
    // reachable from a scripted run declines when it cannot be shown).
    if( SApplication::app().isTestCaseMode() ) {
        TW_LOGW( "ui.servicesui",
                 "purge-unused-clips: confirmation declined (scripted run, no "
                 "rows supplied); %d unused clip(s) kept",
                 int( rows.size() ) );
        return false;
    }

    SPurgeUnusedClipsDialog dlg( parent, rows );
    if( dlg.exec() != QDialog::Accepted ) return false;
    const QList<int> chosen = dlg.chosenRows();
    if( chosen.isEmpty() ) return false;
    if( out ) *out = chosen;
    return true;
}
