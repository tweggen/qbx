#ifndef _SPURGEUNUSEDCLIPSDIALOG_H
#define _SPURGEUNUSEDCLIPSDIALOG_H

#include <QDialog>
#include <QList>
#include <QStringList>

class QCheckBox;
class QPushButton;

/**
 * THE CONFIRMATION IN FRONT OF `purge-unused-clips` (QBX-319).
 *
 * The purge is NOT undoable (model/CONTRACT.md "Loaded orphans" says why), so
 * this dialog is the only place a user can change their mind. Its rules
 * follow SCleanupTakesDialog's, and they are decided, not taste (author,
 * 2026-11-27):
 *
 * 1. ONE CHECKBOX PER CLIP, ALL OFF. The rows are the labels the caller took
 *    from `sunusedclips::list()`, in that order, and `chosenRows()` answers
 *    with indices into the same list. The dialog never reads the project: a
 *    second reading of the list could tick one clip and release another.
 *    Nothing is remembered between openings.
 * 2. CANCEL IS THE DEFAULT, and OK stays disabled until a box is ticked, so
 *    Return can never release anything.
 * 3. IT SAYS WHAT ELSE GOES. A released clip takes with it whatever only it
 *    shows (its sample's file entry, or a missing-media placeholder), and the
 *    release cannot be undone. The text says both.
 * 4. HEADLESS-SAFE. `ask()` never runs an event loop under `--test-case`, and
 *    the answer it gives instead is NO.
 */
class SPurgeUnusedClipsDialog : public QDialog {
    Q_OBJECT

public:
    SPurgeUnusedClipsDialog( QWidget *parent, const QStringList &rows );

    /// The ticked rows, ascending, as indices into the list it was given.
    QList<int> chosenRows() const;
    /// The explanatory text above the rows, for the unit test.
    QString summaryText() const;

    /**
     * Ask, and return TRUE when the user confirmed with at least one row
     * ticked. `*out` receives the ticked rows; it is empty on every FALSE.
     *
     * UNDER `--test-case` IT RETURNS FALSE WITHOUT OPENING ANYTHING. A
     * scripted menu click must not release clips as a side effect of being
     * unable to ask. A case that wants the purge supplies the rows through
     * `purge-unused-clips-gesture`, which goes round this function.
     */
    static bool ask( QWidget *parent, const QStringList &rows,
                     QList<int> *out );

private:
    void refreshOk_();

    QList<QCheckBox *> boxes_;
    QPushButton       *ok_ = nullptr;
};

#endif // _SPURGEUNUSEDCLIPSDIALOG_H
