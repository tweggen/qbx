#include "app/servicesui/scleanuptakesdialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>

#include "tw/core/twlog.h"

#include "app/shell/sapplication.h"

SCleanupTakesDialog::SCleanupTakesDialog( QWidget *parent,
                                         const Summary &summary )
    : QDialog( parent ), summary_( summary )
{
    setWindowTitle( tr( "Clean up takes" ) );
    setModal( true );
    setObjectName( QStringLiteral( "cleanupTakesDialog" ) );

    QVBoxLayout *outer = new QVBoxLayout( this );

    summaryLabel_ = new QLabel( summaryText(), this );
    summaryLabel_->setObjectName( QStringLiteral( "cleanupTakesSummary" ) );
    summaryLabel_->setWordWrap( true );
    summaryLabel_->setMinimumWidth( 380 );
    outer->addWidget( summaryLabel_ );

    // UNCHECKED EVERY TIME, and never remembered. See the header: agreeing to
    // bin files once is not consent to bin files again.
    includeOriginals_ = new QCheckBox(
        tr( "Also remove takes that were not recordings" ), this );
    includeOriginals_->setObjectName(
        QStringLiteral( "cleanupTakesIncludeOriginals" ) );
    includeOriginals_->setToolTip(
        tr( "A take whose name does not carry the recorder's stamp was your "
            "own clip before a recording turned it into a take. Those are "
            "kept unless you ask for them." ) );
    outer->addWidget( includeOriginals_ );

    files_ = new QCheckBox(
        tr( "Move unused recording files to the trash" ), this );
    files_->setObjectName( QStringLiteral( "cleanupTakesFiles" ) );
    files_->setToolTip(
        tr( "Moves the recordings nothing in this project plays to the "
            "system trash. This part cannot be undone inside the "
            "application." ) );
    outer->addWidget( files_ );

    // The one sentence that has to be said out loud: everything else here is
    // one undo away, and that is not.
    QLabel *warn = new QLabel(
        tr( "Removing takes is undoable. Moving files to the trash is not." ),
        this );
    warn->setWordWrap( true );
    outer->addWidget( warn );

    if( !summary_.removedNames.isEmpty() ) {
        // The names, so a false positive on the recorder's name test is
        // something the user can SEE before agreeing rather than discover
        // afterwards. Capped, because a long punch session can leave dozens
        // and an unbounded label would push the buttons off the screen.
        const int kShow = 12;
        QStringList shown = summary_.removedNames.mid( 0, kShow );
        if( summary_.removedNames.size() > kShow )
            shown << tr( "...and %n more", "",
                         summary_.removedNames.size() - kShow );
        QLabel *names = new QLabel( shown.join( QStringLiteral( "\n" ) ),
                                    this );
        names->setObjectName( QStringLiteral( "cleanupTakesNames" ) );
        outer->addWidget( names );
    }

    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
    // The SAFE answer is the default one, as SCleanupDialog's own confirmation
    // already has it: this removes things.
    if( QPushButton *cancel = buttons->button( QDialogButtonBox::Cancel ) )
        cancel->setDefault( true );
    connect( buttons, &QDialogButtonBox::accepted, this, &QDialog::accept );
    connect( buttons, &QDialogButtonBox::rejected, this, &QDialog::reject );
    outer->addWidget( buttons );
}

QString SCleanupTakesDialog::summaryText() const
{
    // EVERY NUMBER HERE CAME IN THROUGH `Summary`. Nothing is recomputed, and
    // nothing is inferred from another field -- see the header's job 1.
    QString s;
    if( summary_.removedTakes > 0 ) {
        s = tr( "Remove %n unused take(s)", "", summary_.removedTakes );
        if( summary_.columnsTouched > 0 )
            s += tr( " across %n clip(s)", "", summary_.columnsTouched );
        s += tr( " from %1." ).arg( summary_.scopeText );
    } else {
        // NOT "nothing to do" -- the caller only opens this dialog when one of
        // the two boxes below can still reach something, so saying nothing is
        // here would contradict the controls underneath it.
        s = tr( "No recorded takes to remove from %1." )
                .arg( summary_.scopeText );
    }
    if( summary_.keptOriginals > 0 )
        s += QStringLiteral( "\n" )
             + tr( "%n take(s) will be kept because they were not recordings. "
                   "The first box below removes those too.",
                   "", summary_.keptOriginals );
    if( summary_.orphanFiles > 0 )
        s += QStringLiteral( "\n" )
             + tr( "%n recording file(s) on disk are not played by this "
                   "project.", "", summary_.orphanFiles );
    return s;
}

SCleanupTakesDialog::Answers SCleanupTakesDialog::answers() const
{
    Answers a;
    a.includeOriginals = includeOriginals_ && includeOriginals_->isChecked();
    a.files            = files_ && files_->isChecked();
    return a;
}

bool SCleanupTakesDialog::ask( QWidget *parent, const Summary &summary,
                               Answers *out )
{
    if( out ) *out = Answers();

    // HEADLESS FIRST, and it answers NO. A `--test-case` run can never dismiss
    // a modal, so this must not open one -- and the answer it gives instead
    // has to be the one that changes nothing, or a scripted menu click would
    // sweep a project's takes as a side effect. A case that wants the sweep
    // supplies the answers itself through `cleanup-takes-gesture`, which does
    // not come through here.
    if( SApplication::app().isTestCaseMode() ) {
        TW_LOGI( "ui.servicesui",
                 "cleanup-takes: confirmation declined (scripted run, no "
                 "answers supplied); would have removed %d take(s)",
                 summary.removedTakes );
        return false;
    }

    SCleanupTakesDialog dlg( parent, summary );
    if( dlg.exec() != QDialog::Accepted ) return false;
    if( out ) *out = dlg.answers();
    return true;
}
