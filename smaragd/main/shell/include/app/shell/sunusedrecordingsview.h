#ifndef _SUNUSEDRECORDINGSVIEW_H_
#define _SUNUSEDRECORDINGSVIEW_H_

#include <QStringList>
#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;

/**
 * THE "UNUSED RECORDINGS" DOCK (plan 53 §6.3, P6) — read-only.
 *
 * This is where X7 ends. Three things can leave a capture file with nothing
 * playing it: a lane REFUSED the pass (§5.4, which QBX-164 made reportable), the
 * backend never wrote the file, or the user simply UNDID the take. In every
 * case the audio is on disk and, until this dock, the user's only notice was a
 * `qWarning` nobody reads — their take appeared to have evaporated.
 *
 * TWO SOURCES, AND THEY ARE DIFFERENT QUESTIONS:
 *
 *  - `srecfiles::scanUnused( project )` — what the project holds that nothing
 *    places, asked fresh every time the list is built. A question about NOW,
 *    so an undo that puts a take back takes its file off the list.
 *  - THE SESSION'S SKIPPED LANES, accumulated here as they are reported. The
 *    recorder's own `skippedLanes()` is reset at the start of every pass (its
 *    header says so), and what a user wants is everything this session lost,
 *    not just the last press. Accumulating in the VIEW rather than widening
 *    the recorder is deliberate: "what have I been told so far" is view state,
 *    and the recorder has no business growing a session-lifetime list for a
 *    dock to read.
 *
 * IT HOLDS NO POINTER INTO THE PROJECT — only strings — so unlike the mixer
 * pane (shell/CONTRACT.md inv. 61) it needs no entry on
 * `destroyDocksToolbars()`'s detach list: there is nothing here to dereference
 * after `closeProject()` deletes the project. That is a property worth stating
 * rather than discovering, because the obvious implementation (keep the
 * `SExternFile *` you found, so the row can act on it) would have exactly the
 * crash inv. 61 records.
 *
 * READ-ONLY, and that is §6.3's design rather than a shortcut: the list is
 * "the same scan without the removal", and the removal is `cleanup-takes`. A
 * dock that could bin a file would be a second route to a destructive
 * operation, with its own confirmation to get wrong.
 */
class SUnusedRecordingsView : public QWidget
{
    Q_OBJECT
public:
    explicit SUnusedRecordingsView( QWidget *parent = nullptr );

public slots:
    /** Re-run the scan and repopulate. Cheap, idempotent, and safe with no
     *  project (it then lists only what the session reported). */
    void refresh();

    /**
     * Record what a pass lost, from `SApplication::recordingLanesSkipped`.
     *
     * It pulls the full list off the recorder at that moment rather than
     * trusting the signal's two arguments: the signal carries a COUNT and the
     * FIRST reason, which is all a status-bar line needs, while this dock
     * wants every lane and its own reason.
     */
    void onRecordingLanesSkipped( int count, const QString &firstWhy );

private:
    QListWidget *list_    = nullptr;
    QLabel      *summary_ = nullptr;
    QPushButton *refresh_ = nullptr;

    /// Everything this SESSION reported as skipped, newest last. Strings only
    /// (see the class comment).
    QStringList sessionSkipped_;
};

#endif // _SUNUSEDRECORDINGSVIEW_H_
