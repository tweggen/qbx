#ifndef SCLIPPROPERTIESPANEL_H
#define SCLIPPROPERTIESPANEL_H

#include <QList>
#include <QSet>
#include <QStringList>
#include <QWidget>

class SCut;
class SLink;
class SCompositeAction;

class QCheckBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;

/**
 * Clip properties panel: the numeric/textual face of the clip window that the
 * timeline only exposes as drag gestures. Shows the CURRENT selection
 * (SApplication's selection list) and commits every edit through the SAME
 * actions the gestures use — so a typed value and a dragged edge are the same
 * undo step kind and honour take stacks and edit-group broadcast identically.
 *
 * Selection handling: the panel NEVER caches an SLink*. Selected links can be
 * destroyed by any action, and holding one across calls is exactly the
 * use-after-destruction class this codebase has been burned by. refresh()
 * re-reads SApplication::getCurrentSelectionPaths() and resolves path -> link
 * -> SCut from scratch every single time.
 *
 * Multi-select is ABSOLUTE-set semantics: a field showing one shared value
 * writes that value to every selected clip; a field whose clips disagree reads
 * BLANK and is ignored unless the user types into it. One edit over N clips is
 * ONE SCompositeAction, i.e. one undo step.
 *
 * Refresh-while-editing: refresh() is meant to hang off
 * SProject::arrangementChanged, which fires for every committed action —
 * including the panel's own. Hence updating_ (re-entrancy), blocked signals
 * around every programmatic write, commits only on editingFinished/clicked
 * (never valueChanged/textChanged), a "the user actually touched this" gate,
 * and no setFocus() from refresh().
 */
class SClipPropertiesPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SClipPropertiesPanel( QWidget *parent = nullptr );
    ~SClipPropertiesPanel() override;

    // Keyboard focus to the first enabled editable field (the dock's F2 entry
    // point). Deliberately NOT called from refresh() — stealing focus on every
    // committed action would fight the user.
    void focusFirstField();

public slots:
    // Re-read the selection and repopulate every field. Cheap and idempotent.
    void refresh();

private slots:
    void commitName();
    void commitStartTime();
    void commitDuration();
    void commitSlip();
    void commitStretch();
    void commitPitch();
    void commitLoopLength();
    void onClearLoop();
    void onClearWarp();
    void onFormantClicked( bool checked );

private:
    // One resolved selection entry. Built fresh on every use and never stored
    // in a member — see the class comment.
    struct ClipRef {
        QList<int> path;              // track path + link index
        SLink     *link = nullptr;    // the placement (owns the timeline start)
        SCut      *cut  = nullptr;    // the clip, or a stack's ACTIVE take
        int        takeIndex = -1;    // -1 = plain clip, else active take index
        int        nTakes    = 0;     // 0 = plain clip
    };
    QList<ClipRef> collectClips() const;

    void buildUi();

    // Display helpers: write one value per selected clip into a widget, showing
    // the shared value when they agree and a BLANK field when they do not.
    // Signals are blocked throughout, so a refresh can never look like an edit.
    void showValues( QSpinBox *sp, const QList<qint64> &values );
    void showValues( QDoubleSpinBox *sp, const QList<double> &values );
    void showValues( QLineEdit *le, const QStringList &values );
    void showValues( QCheckBox *cb, const QList<bool> &values );

    // True iff the user actually interacted with `w` since the last commit;
    // consumes the mark. Guards against QAbstractSpinBox restoring the text of
    // a blanked (mixed) field on focus-out and thereby committing a value the
    // user never typed.
    bool consumeEdited( QObject *w );
    void markEdited( QObject *w ) { edited_.insert( w ); }

    // Submit iff non-empty, otherwise destroy: a composite with zero children
    // fails apply(), and a failing apply breaks the headless test cases.
    void submitComposite( SCompositeAction *composite );

    // Re-entrancy guard for refresh() AND a hard "this write is programmatic"
    // flag every commit slot checks.
    bool updating_ = false;

    QSet<QObject*> edited_;

    QLabel      *placeholder_    = nullptr;   // "No clip selected"
    QScrollArea *scroll_         = nullptr;   // holds the whole form

    // Source group (read-only, plus the two destructive one-shot buttons).
    QLabel      *sourceLabel_    = nullptr;
    QLabel      *takesLabel_     = nullptr;
    QLabel      *warpLabel_      = nullptr;
    QPushButton *clearWarpButton_ = nullptr;

    // Editable fields.
    QLineEdit      *nameEdit_    = nullptr;
    QSpinBox       *startSpin_   = nullptr;
    QSpinBox       *durationSpin_ = nullptr;
    QSpinBox       *slipSpin_    = nullptr;
    QDoubleSpinBox *stretchSpin_ = nullptr;
    QSpinBox       *pitchSpin_   = nullptr;
    QLabel         *pitchStLabel_ = nullptr;  // semitone readout next to pitch
    QSpinBox       *loopSpin_    = nullptr;
    QPushButton    *clearLoopButton_ = nullptr;
    QCheckBox      *formantCheck_ = nullptr;
};

#endif // SCLIPPROPERTIESPANEL_H
