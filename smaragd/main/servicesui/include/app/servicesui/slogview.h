#ifndef SLOGVIEW_H
#define SLOGVIEW_H

#include <QWidget>

class SLogModel;

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QTableView;
class QTimer;
class QToolButton;

// The log dock's contents (proposal 24 §6.2).
//
// A QTableView configured so that layout cost is independent of row count:
// fixed row height (so scrollbar geometry is arithmetic and painting is O(visible
// rows)), and the four narrow columns sized ONCE and then pinned Fixed --
// ResizeToContents on a growing model is O(N) per insert and would dominate
// everything else the dock does.
class SLogView : public QWidget
{
    Q_OBJECT
public:
    explicit SLogView( QWidget *parent = nullptr );

    // Wired to QDockWidget::visibilityChanged: a closed log drains nothing.
    void setLive( bool live );

    // Records in the sink the view has not yet absorbed. Used by the
    // log-stress test action to tell "caught up" from "still catching up".
    int  backlog() const;
    int  displayedRows() const;
    qint64 worstDrainMs() const;
    void   resetDrainStats();

private slots:
    void onRowsAppended();
    void onTextFilterEdited();
    void applyTextFilter();
    void onLevelChanged( int index );
    void rebuildCategoryMenu();
    void onCategoryToggled();
    void togglePause( bool paused );
    void clearView();
    void copySelection();
    void saveAs();
    void openLogFolder();
    void refreshStatus();

private:
    bool atBottom() const;
    void scrollToBottom();

    SLogModel   *model_       = nullptr;
    QTableView  *table_       = nullptr;
    QComboBox   *levelBox_    = nullptr;
    QToolButton *categoryBtn_ = nullptr;
    QMenu       *categoryMenu_ = nullptr;
    QLineEdit   *filterEdit_  = nullptr;
    QAction     *pauseAct_    = nullptr;
    QLabel      *statusLabel_ = nullptr;

    QTimer *filterDebounce_ = nullptr;   // avoid a rescan per keystroke
    QTimer *statusTimer_    = nullptr;
    QTimer *categoryTimer_  = nullptr;   // pick up categories as they appear

    bool wantLive_ = false;              // what the dock asked for
    bool paused_   = false;              // what the user asked for
};

#endif // SLOGVIEW_H
