// THE CLEAN-UP CONFIRMATION (plan 53 §6.3, QBX-195).
//
// WHY A UNIT TEST AND NOT A .qxa. The dialog is MODAL, and a modal in a
// `--test-case` run is a hang, not a failure -- so `SCleanupTakesDialog::ask`
// never opens one there and answers "no" instead. That is the right behaviour
// (see its header), and it means a scripted run can gate the GESTURE but can
// never see the dialog's own surface. `cleanup_takes_gesture.qxa` covers the
// gesture; this covers the widget, constructed directly, which is the same
// split render_dialog_test makes.
//
// WHAT IT PINS, and the first two are the ticket's acceptance criteria 2 and 3:
//
//   a. THE NUMBERS ON SCREEN ARE THE ONES IT WAS GIVEN. Every figure comes out
//      of the `Summary` the caller filled from `scleanuptakes::plan` -- the
//      same walk the sweep then runs. A dialog that counted for itself would
//      be a second reading of decision D5 (a)'s "did this come from a
//      recording" name test, and a confirmation that lied about the next
//      click.
//   b. BOTH DANGEROUS BOXES DEFAULT TO OFF, and `answers()` reports them so.
//      Nothing is moved to the trash, and no original clip is removed, without
//      the user having ticked something.
//   c. THE ZERO CASE still offers the boxes rather than claiming there is
//      nothing here. The caller opens this dialog when EITHER box can still
//      reach something -- an original take, or an orphan file -- so a summary
//      reading "nothing to do" over two live controls would contradict them.
//      This is the case the gesture got wrong on its first run: it bailed on
//      "no recorded takes" and made the checkbox unreachable.
//   d. THE NAME LIST IS CAPPED, so a long punch session cannot push the
//      buttons off the screen, and the cap ANNOUNCES itself.
//   e. CANCEL IS THE DEFAULT BUTTON. This removes things; Return must not.

#include "app/servicesui/scleanuptakesdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QString>

#include <cstdio>

static int gFailures = 0;

static void check( bool cond, const char *what )
{
    if( cond ) {
        std::printf( "PASS %s\n", what );
    } else {
        std::printf( "FAIL %s\n", what );
        ++gFailures;
    }
}

static void checkContains( const QString &haystack, const QString &needle,
                           const char *what )
{
    const bool ok = haystack.contains( needle );
    if( !ok )
        std::printf( "  ...looked for \"%s\" in \"%s\"\n",
                     needle.toLocal8Bit().constData(),
                     haystack.toLocal8Bit().constData() );
    check( ok, what );
}

int main( int argc, char **argv )
{
    QApplication app( argc, argv );
    std::printf( "=== SCleanupTakesDialog ===\n" );

    // ---- a. the numbers, and only the numbers it was handed ---------------
    {
        SCleanupTakesDialog::Summary s;
        s.removedTakes   = 3;
        s.keptOriginals  = 1;
        s.columnsTouched = 2;
        s.orphanFiles    = 4;
        s.removedNames   = { QStringLiteral( "Take 1 · 12:00" ),
                             QStringLiteral( "Take 2 · 12:00" ),
                             QStringLiteral( "Take 3 · 12:01" ) };
        s.scopeText      = QStringLiteral( "2 selected clips" );

        SCleanupTakesDialog dlg( nullptr, s );
        const QString text = dlg.summaryText();
        std::printf( "  summary: %s\n", text.toLocal8Bit().constData() );
        // WHOLE PHRASES, not bare digits. A bare "1" matched this summary even
        // when the kept-originals sentence was missing from it entirely --
        // measured, on the first run of this test -- because "clip(s)" and the
        // other counts supply digits of their own. A count has to be asserted
        // together with what it is a count OF.
        checkContains( text, QStringLiteral( "Remove 3 unused take(s)" ),
                       "the take count is shown, with its noun" );
        checkContains( text, QStringLiteral( "across 2 clip(s)" ),
                       "...and how many columns it is spread over" );
        checkContains( text, QStringLiteral( "2 selected clips" ),
                       "the scope the caller named is shown verbatim" );
        checkContains( text, QStringLiteral( "1 take(s) will be kept" ),
                       "the kept-originals count is shown, with its noun" );
        checkContains( text, QStringLiteral( "4 recording file(s) on disk" ),
                       "the orphan FILE count is shown - a different question "
                       "from the take count, and a different noun" );

        // ---- b. both boxes off, and answers() says so -------------------
        QCheckBox *inc = dlg.findChild<QCheckBox *>(
            QStringLiteral( "cleanupTakesIncludeOriginals" ) );
        QCheckBox *files = dlg.findChild<QCheckBox *>(
            QStringLiteral( "cleanupTakesFiles" ) );
        check( inc && !inc->isChecked(),
               "\"also remove non-recordings\" starts UNCHECKED" );
        check( files && !files->isChecked(),
               "\"move files to the trash\" starts UNCHECKED" );
        check( !dlg.answers().includeOriginals && !dlg.answers().files,
               "answers() reports both off before anything is ticked" );

        // ...and ticking is what changes the answer. Without this the pair
        // above would also hold on a dialog whose boxes do nothing.
        if( inc ) inc->setChecked( true );
        if( files ) files->setChecked( true );
        check( dlg.answers().includeOriginals && dlg.answers().files,
               "answers() follows the boxes once they are ticked" );

        // ---- e. Cancel is the default button ----------------------------
        QDialogButtonBox *box = dlg.findChild<QDialogButtonBox *>();
        QPushButton *cancel = box
            ? box->button( QDialogButtonBox::Cancel ) : nullptr;
        QPushButton *ok = box ? box->button( QDialogButtonBox::Ok ) : nullptr;
        check( cancel && cancel->isDefault(),
               "Cancel is the DEFAULT button - Return must not sweep" );
        check( ok && !ok->isDefault(), "...and OK is not" );
    }

    // ---- c. the zero case still offers the boxes --------------------------
    {
        SCleanupTakesDialog::Summary s;
        s.removedTakes  = 0;        // nothing the DEFAULT plan would remove
        s.keptOriginals = 1;        // ...but one the first box reaches
        s.scopeText     = QStringLiteral( "the whole project" );

        SCleanupTakesDialog dlg( nullptr, s );
        const QString text = dlg.summaryText();
        checkContains( text, QStringLiteral( "the whole project" ),
                       "the zero case still names its scope" );
        checkContains( text, QStringLiteral( "No recorded takes to remove" ),
                       "the zero case says what it is, rather than "
                       "borrowing the non-zero sentence" );
        checkContains( text, QStringLiteral( "first box below removes those" ),
                       "...and points at the box that CAN reach the take, "
                       "rather than claiming there is nothing here" );
        check( dlg.findChild<QCheckBox *>(
                   QStringLiteral( "cleanupTakesIncludeOriginals" ) ) != nullptr,
               "the box is present in the zero case - it is the only way out "
               "of it" );
    }

    // ---- d. the name list is capped, and says so --------------------------
    {
        SCleanupTakesDialog::Summary s;
        s.removedTakes = 30;
        s.scopeText    = QStringLiteral( "the whole project" );
        for( int i = 0; i < 30; ++i )
            s.removedNames << QStringLiteral( "Take %1 · 12:00" ).arg( i );

        SCleanupTakesDialog dlg( nullptr, s );
        QLabel *names = dlg.findChild<QLabel *>(
            QStringLiteral( "cleanupTakesNames" ) );
        check( names != nullptr, "a long sweep still lists names" );
        if( names ) {
            const int lines = names->text().count( QLatin1Char( '\n' ) ) + 1;
            check( lines <= 13,
                   "the list is capped rather than 30 lines tall" );
            checkContains( names->text(), QStringLiteral( "more" ),
                           "...and the cap ANNOUNCES itself" );
            checkContains( names->text(), QStringLiteral( "Take 0" ),
                           "the first names are the ones shown" );
            check( !names->text().contains( QStringLiteral( "Take 29" ) ),
                   "the last ones are not" );
        }
    }

    std::printf( "%s (%d failure(s))\n", gFailures == 0 ? "OK" : "FAILED",
                 gFailures );
    return gFailures == 0 ? 0 : 1;
}
