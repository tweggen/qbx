#ifndef _SCLEANUPTAKESDIALOG_H
#define _SCLEANUPTAKESDIALOG_H

#include <QDialog>
#include <QString>
#include <QStringList>

class QCheckBox;
class QLabel;

/**
 * THE CONFIRMATION IN FRONT OF `cleanup-takes` (plan 53 §6.3, QBX-195).
 *
 * P6 shipped the sweep with no command behind it, so nothing ever had to ask
 * the user anything. This is that question, and it has three jobs.
 *
 * 1. IT STATES WHAT WILL HAPPEN, IN NUMBERS IT DID NOT COMPUTE. Everything on
 *    screen comes out of a `Summary` the caller filled from
 *    `scleanuptakes::plan` -- the same walk the sweep itself then runs. A
 *    dialog that counted for itself would be a second reading of decision
 *    D5 (a)'s "did this take come from a recording" name test, and the failure
 *    when the two drifted would be a confirmation that lied about the next
 *    click. (It is also a layering fact: `app/servicesui` may not depend on
 *    `app/objects/cut`, so this cannot ask even if it wanted to. The caller --
 *    the arranger, which may see both -- is the one that asks.)
 *
 * 2. IT MAKES THE TWO DANGEROUS OPTIONS VISIBLE AND OFF. `includeOriginals`
 *    removes takes that were the user's OWN clips rather than recordings, and
 *    `files` moves orphan recordings to the OS trash -- and the file half is
 *    the one part of the sweep that is NOT on the undo stack. Both are
 *    unchecked every time this dialog opens: they are never remembered, which
 *    is deliberate, because "I agreed to that last week" is not consent to
 *    binning a file today.
 *
 * 3. IT IS HEADLESS-SAFE. `ask()` never runs an event loop under
 *    `--test-case`: a modal nobody can dismiss is a hang, and that is a
 *    project rule rather than a local nicety. See `ask()`'s own note for what
 *    it answers instead and why that answer is "cancel".
 */
class SCleanupTakesDialog : public QDialog {
    Q_OBJECT

public:
    /**
     * What the sweep would do, as the caller measured it. Nothing in here is
     * derived by the dialog.
     */
    struct Summary {
        int         removedTakes   = 0;
        int         keptOriginals  = 0;
        int         columnsTouched = 0;
        /// Orphan recording FILES on disk (`srecfiles::scanUnused`), which is
        /// a different count from the takes: the file half acts on files the
        /// project does not reference at all.
        int         orphanFiles    = 0;
        /// The names of the takes that would go, for the detail list.
        QStringList removedNames;
        /// One phrase naming the scope, already user-facing: "the whole
        /// project", "3 selected clips", "2 tracks". The caller knows which
        /// surface invoked the sweep; the dialog does not.
        QString     scopeText;
    };

    /**
     * What the user answered. Both default to off, which is also what a
     * cancelled or headless call leaves them as.
     */
    struct Answers {
        bool includeOriginals = false;
        bool files            = false;
    };

    SCleanupTakesDialog( QWidget *parent, const Summary &summary );

    /** The text the dialog is showing, for a unit test and for the log line. */
    QString summaryText() const;
    Answers answers() const;

    /**
     * Put the question and return TRUE when the user said yes.
     *
     * UNDER `--test-case` IT RETURNS FALSE WITHOUT OPENING ANYTHING, and
     * "false" rather than "true" is the load-bearing half: a scripted run that
     * reached a confirmation nobody scripted must not have its takes swept as
     * a side effect of a menu click. A case that WANTS the sweep supplies the
     * answers itself (`cleanup-takes-gesture`), which goes round this function
     * rather than through it. The refusal is logged, so a case that expected
     * the sweep and got nothing can see why.
     *
     * With nothing to remove the caller should not reach here at all -- a
     * sweep over a tidy project is a no-op that says so, not a dialog.
     */
    static bool ask( QWidget *parent, const Summary &summary,
                     Answers *out );

private:
    Summary    summary_;
    QLabel    *summaryLabel_     = nullptr;
    QCheckBox *includeOriginals_ = nullptr;
    QCheckBox *files_            = nullptr;
};

#endif // _SCLEANUPTAKESDIALOG_H
