// THE PURGE CONFIRMATION (QBX-319).
//
// A UNIT TEST AND NOT A .qxa for cleanup_takes_dialog_test's reason: the
// dialog is MODAL, so `SPurgeUnusedClipsDialog::ask` never opens one under
// `--test-case` and answers NO. purge_unused_clips.qxa gates the gesture and
// that decline; this gates the widget, constructed directly.
//
// WHAT IT PINS (the author's decisions, 2026-11-27):
//   a. ONE CHECKBOX PER ROW, labelled with the row it was given, in order.
//   b. EVERY BOX STARTS OFF, so `chosenRows()` is empty until one is ticked.
//   c. `chosenRows()` answers with indices into the list it was given.
//   d. CANCEL IS THE DEFAULT, and OK is disabled until a box is ticked.
//   e. THE TEXT SAYS what else goes with a clip, and that it cannot be undone.

#include "app/servicesui/spurgeunusedclipsdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QString>
#include <QStringList>

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

int main( int argc, char **argv )
{
    QApplication app( argc, argv );
    std::printf( "=== SPurgeUnusedClipsDialog ===\n" );

    const QStringList rows = {
        QStringLiteral( "a.wav, 1.000 s from 0.000 s" ),
        QStringLiteral( "a.wav, 1.500 s from 0.500 s" ),
        QStringLiteral( "b.wav, 0.500 s from 2.000 s" ) };
    SPurgeUnusedClipsDialog dlg( nullptr, rows );

    // a. one box per row, in order, with the row's text.
    bool labelled = true;
    for( int i = 0; i < rows.size(); ++i ) {
        QCheckBox *b = dlg.findChild<QCheckBox *>(
            QStringLiteral( "purgeUnusedClipsRow%1" ).arg( i ) );
        if( !b || b->text() != rows.at( i ) ) labelled = false;
    }
    check( labelled, "a. one checkbox per row, labelled in order" );
    check( dlg.findChildren<QCheckBox *>().size() == rows.size(),
           "a. no checkbox besides the rows" );

    // b. all off.
    bool allOff = true;
    for( QCheckBox *b : dlg.findChildren<QCheckBox *>() )
        if( b->isChecked() ) allOff = false;
    check( allOff, "b. every box starts unchecked" );
    check( dlg.chosenRows().isEmpty(), "b. nothing chosen before a tick" );

    // d. defaults.
    QPushButton *ok = dlg.findChild<QPushButton *>(
        QStringLiteral( "purgeUnusedClipsOk" ) );
    QDialogButtonBox *box = dlg.findChild<QDialogButtonBox *>();
    QPushButton *cancel = box ? box->button( QDialogButtonBox::Cancel )
                              : nullptr;
    check( cancel && cancel->isDefault(), "d. Cancel is the default button" );
    check( ok && !ok->isDefault(), "d. OK is not the default button" );
    check( ok && !ok->isEnabled(), "d. OK is disabled with nothing ticked" );

    // c. ticking rows 2 and 0 answers {0, 2}.
    dlg.findChild<QCheckBox *>( QStringLiteral( "purgeUnusedClipsRow2" ) )
        ->setChecked( true );
    dlg.findChild<QCheckBox *>( QStringLiteral( "purgeUnusedClipsRow0" ) )
        ->setChecked( true );
    check( dlg.chosenRows() == QList<int>( { 0, 2 } ),
           "c. chosenRows() is the ticked indices, ascending" );
    check( ok && ok->isEnabled(), "d. OK is enabled once a box is ticked" );
    dlg.findChild<QCheckBox *>( QStringLiteral( "purgeUnusedClipsRow0" ) )
        ->setChecked( false );
    dlg.findChild<QCheckBox *>( QStringLiteral( "purgeUnusedClipsRow2" ) )
        ->setChecked( false );
    check( ok && !ok->isEnabled(), "d. OK is disabled again when untick'd" );

    // e. the text.
    const QString s = dlg.summaryText();
    check( s.contains( QStringLiteral( "cannot be undone" ) ),
           "e. the text says the purge cannot be undone" );
    check( s.contains( QStringLiteral( "only that clip shows" ) ),
           "e. the text says what else goes with a clip" );

    std::printf( "%d failure(s)\n", gFailures );
    return gFailures ? 1 : 0;
}
