#ifndef _SEVENTEDITORDOCK_H_
#define _SEVENTEDITORDOCK_H_

#include <QList>
#include <QString>
#include <QWidget>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QShowEvent;
class QToolButton;
class QVBoxLayout;

class SEventEditorView;
class SEventTimeAxis;
class SEventTimeRuler;
class SPianoRollView;

/**
 * SEventEditorDock - the event editor's widget (proposal 37 6.2). The fifth
 * QDockWidget, bottom, tabified with the Log; the dock itself is created in
 * SMainWindow's constructor, like every other one (shell/CONTRACT invariant 4:
 * restoreWindowLayout() can only restore docks that already exist).
 *
 * It is a SELECTION FOLLOWER, the exact SClipPropertiesPanel shape (timeline
 * invariants 8 and 9):
 *   - refresh() re-reads SApplication::getCurrentSelectionPaths() and
 *     re-resolves path -> link -> event clip from scratch; nothing here ever
 *     holds an SLink*;
 *   - it hangs off SProject::arrangementChanged, which every committed action
 *     ends in - INCLUDING the editor's own edits, so refresh() has to survive
 *     being called by its own commit (updating_ + blocked signals + no
 *     setFocus from the refresh path).
 *
 * It owns the toolbar, the ruler and the shared time axis; the VIEW comes from
 * the kind registry and knows nothing about any of them. That is what makes a
 * tracker grid or a score view a pure addition later.
 */
class SEventEditorDock : public QWidget
{
    Q_OBJECT
public:
    explicit SEventEditorDock( QWidget *parent = nullptr );
    ~SEventEditorDock() override;

    /** The shared axis. The shell links it to the arranger's zoom/scroll. */
    SEventTimeAxis *timeAxis() const { return axis_; }

    /** The current view, or null before a kind is chosen. */
    SEventEditorView *view() const { return view_; }
    /** The current view iff it is the piano roll (what `drag-note` drives). */
    SPianoRollView *pianoRoll() const;

    /** Switch editor kind. A no-op for an unknown key. */
    void setKind( const QString &kind );
    QString kind() const { return kind_; }

    /** The clip the editor is bound to; empty when it shows nothing. */
    const QList<int> &clipPath() const { return clipPath_; }

    /**
     * The test face (`assert-event-editor`). Delegates to the view, so the
     * string is the view's describe() verbatim; with no view at all it reports
     * the empty form, which is what an audio-clip selection produces.
     */
    QString describe() const;

    /**
     * Bind to an explicit clip instead of the selection. The testkit's route
     * (`assert-event-editor clip=`), so a case can describe a clip it has not
     * selected; an empty path means "follow the selection", i.e. refresh().
     */
    void bindClip( const QList<int> &clipPath );

public slots:
    /** Re-resolve the selection and rebind. Cheap and idempotent. */
    void refresh();

protected:
    /**
     * ACTIVELY re-query the selection whenever this dock actually becomes
     * visible - toggled open from the View menu, raised from a tab group, or
     * shown for the first time - rather than relying only on the reactive
     * arrangementChanged -> refresh() connection SMainWindow wires. That
     * connection is set up once, by SMainWindow::attachEventEditor(); a dock
     * that is merely SHOWN again had no reason to have missed any selection
     * change in between, but this is the defensive half of that argument
     * (SMainWindow::showEventEditor() is the ACTIVE half every production
     * "open" gesture goes through).
     */
    void showEvent( QShowEvent *event ) override;

private slots:
    void onToolChanged();
    void onGridChanged( int index );
    void onKindChanged( int index );
    void onLinkToggled( bool on );
    void onQuantizeClicked();
    void onCcLaneClicked();

private:
    void buildUi();
    /** The first EVENT clip in the current selection, or an empty path. */
    QList<int> resolveSelectedEventClip() const;
    void applyGridToChildren();

    SEventTimeAxis  *axis_   = nullptr;
    SEventTimeRuler *ruler_  = nullptr;
    SEventEditorView *view_  = nullptr;
    QVBoxLayout     *viewSlot_ = nullptr;
    QLabel          *placeholder_ = nullptr;

    QToolButton *toolSelect_ = nullptr;
    QToolButton *toolDraw_   = nullptr;
    QToolButton *toolErase_  = nullptr;
    QComboBox   *gridCombo_  = nullptr;
    QComboBox   *kindCombo_  = nullptr;
    QComboBox   *ccCombo_    = nullptr;
    QPushButton *ccButton_   = nullptr;
    QPushButton *quantize_   = nullptr;
    // "Q" = quantize, scoped to this dock (WidgetWithChildrenShortcut) so it
    // cannot shadow the virtual keyboard's own Q = note-12 binding when THAT
    // dock has focus (svirtualkeyboarddock.cpp) — see buildUi().
    QAction     *actQuantize_ = nullptr;
    QCheckBox   *linkCheck_  = nullptr;

    QString    kind_;
    QList<int> clipPath_;
    // Re-entrancy: refresh() runs off arrangementChanged, which the dock's own
    // committed edits emit (timeline invariant 9).
    bool       updating_ = false;
};

#endif // _SEVENTEDITORDOCK_H_
