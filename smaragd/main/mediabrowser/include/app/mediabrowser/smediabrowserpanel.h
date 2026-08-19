// app/mediabrowser/smediabrowserpanel.h — the Media Browser dock's one widget.
//
// Proposal 38 gate 2 (§B.4, §B.5). Source combo, media-type checkbox menu,
// editable path with an "up" button, a debounced search box with a recurse
// checkbox, a tree that is BOTH the browse tree and the flat search result
// list, an inline error banner with Retry, and a footer carrying the counts,
// the truncation and the busy flag.
//
// Read main/mediabrowser/CONTRACT.md before changing anything here. Three
// things in particular:
//
//   * SUPERSESSION IS BY REQUEST ID (app/media inv. 2). The panel keeps the id
//     of the request it is DISPLAYING and drops any batch whose id is not one
//     it is waiting for. A late result from a superseded search therefore
//     cannot repaint over a newer one BY CONSTRUCTION — never by a cancel
//     winning a race.
//   * NOTHING IS CONTACTED AT CONSTRUCTION. A source is registered (which does
//     no I/O) and remembered, and it is OPENED when it is SELECTED — by the
//     combo, by a test verb, or by the dock first becoming visible. A dock
//     restored with media/lastSourceId pointing at a dead server costs a
//     banner at the next click, never a hang at launch (§B.4).
//   * THE PANEL IS BUILT OFF SCREEN AND NEVER SHOWN under `--test-case`
//     (trap T10), so every test verb pushes it its state explicitly and
//     describe() is the oracle. Nothing here may depend on having been painted.

#ifndef SMEDIABROWSERPANEL_H
#define SMEDIABROWSERPANEL_H

// SMediaEntry by VALUE, not forward-declared: the batch slot takes a
// QVector<SMediaEntry> and moc has to see the complete type to build the
// argument metatype for it.
#include "app/media/smediaref.h"

#include <QHash>
#include <QVariant>
#include <QString>
#include <QStringList>
#include <QTreeWidget>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QMimeData;
class QPushButton;
class QTimer;
class QToolButton;
class QTreeWidgetItem;

class SDelayedLocalSource;
class SLocalMediaSource;
class SMediaSource;
class SMediaBrowserPanel;

// A compact byte-size spelling: at most 3 significant digits, plus an
// optional decimal (1000-based, §k) k/M/G unit-prefix suffix — never "B", and
// never a prefix letter under 1000 bytes. `bytes < 0` (the panel's own
// "unknown", e.g. a directory row) is the empty string, matching the size
// column's existing convention. Free so `mediabrowser_size_test` can gate it
// without building the whole panel widget.
QString mediaBrowserFormatSize( qint64 bytes );

// The tree, subclassed for ONE reason: QTreeWidget::startDrag is protected and
// builds its own item MIME, and this dock must emit exactly the payload the
// arranger's drop handler already understands ("file:<abspath>",
// application/x-smaragd-resource — §A.1). No Q_OBJECT: it declares no signal
// and no slot, so it needs no meta-object of its own.
class SMediaBrowserTree : public QTreeWidget
{
public:
    SMediaBrowserTree( SMediaBrowserPanel *owner, QWidget *parent );

protected:
    void startDrag( Qt::DropActions supportedActions ) override;

private:
    SMediaBrowserPanel *owner_ = nullptr;
};

class SMediaBrowserPanel : public QWidget
{
    Q_OBJECT

public:
    explicit SMediaBrowserPanel( QWidget *parent = nullptr );
    ~SMediaBrowserPanel() override;

    // --- the test/shell entry points (all of them push state EXPLICITLY) ---
    //
    // Every one of these is what the matching verb drives, and every one of
    // them goes through the same code path the widget's own signal would: a
    // verb that wrote the panel's fields directly would pass while the control
    // was broken (testkit CONTRACT inv. 5).

    /// Select a source by id and OPEN it. False when no such source is
    /// registered — which is the "unreachable source" path, banner included.
    bool selectSource( const QString &sourceId );
    /// Set the browse root. Leaves search mode if it was active.
    bool setBrowsePath( const QString &path );
    /// Expand the directory row named `name` (the REAL itemExpanded path, so
    /// the lazy listDirectory that a user's click issues is what runs).
    bool expandRowNamed( const QString &name );
    /// Enter search mode (or, with an empty needle, return to browse at the
    /// same path). `viaDebounce` types into the real line edit and lets the
    /// 250 ms timer fire; otherwise the debounce is flushed at once, which is
    /// what keeps a case deterministic.
    bool setSearch( const QString &needle, bool recursive, bool viaDebounce );
    /// "audio", "midi", "audio,midi", "" — the checkbox menu's state.
    bool setCategories( const QString &spelling );

    /// The MIME a drag out of `row` (depth-first index, matching describe())
    /// or of the first row named `name` would carry. NULL for a directory row,
    /// an out-of-range row or an unknown name — a directory is not draggable
    /// (§B.5). Ownership passes to the caller.
    QMimeData *createDragMime( int row, const QString &name ) const;

    /// One line of state plus one `name,size,dir` triple per row. THE oracle:
    /// see CONTRACT.md for the exact format, which `assert-media-browser`
    /// matches against.
    QString describe() const;

    /// True while any request this panel is displaying is still running — the
    /// root listing/search, a lazy expand, or a search still inside its
    /// debounce window. A test verb waits on this rather than sleeping.
    bool isBusy() const;

protected:
    // The first time the dock is actually shown is the first time a source may
    // be contacted (§B.4). A headless run never shows it, which is exactly
    // what keeps the qxa cases from opening anything a verb did not ask for.
    void showEvent( QShowEvent *event ) override;

private slots:
    void onSourceComboActivated( int index );
    void onRegistryChanged();
    void onPathEdited();
    void onUpClicked();
    void onSearchTextChanged();
    void onDebounceFired();
    void onRecursiveToggled( bool on );
    void onFilterMenuChanged();
    void onItemExpanded( QTreeWidgetItem *item );
    void onRetryClicked();

    void onEntriesReady( int requestId, const QVector<SMediaEntry> &batch,
                         bool final, int truncatedCount );
    void onRequestFailed( int requestId, const QString &message );

private:
    friend class SMediaBrowserTree;

    enum Mode { Browse, Search };

    void buildUi();
    void registerBuiltinSources();
    void reloadSourceCombo();
    void restoreState();

    /// Resolve sourceId_ through the registry and issue the first request.
    /// The ONLY place a source is contacted.
    void openCurrentSource();
    /// Cancel everything in flight, clear the tree, and issue the request the
    /// current mode/path/needle describe.
    void refreshRoot();
    void cancelAll();

    void appendEntries( QTreeWidgetItem *parent,
                        const QVector<SMediaEntry> &batch );
    QTreeWidgetItem *makeItem( const SMediaEntry &entry ) const;
    void collectRows( QTreeWidgetItem *parent, QVector<QTreeWidgetItem *> &out ) const;
    QVector<QTreeWidgetItem *> visibleRows() const;

    QMimeData *mimeForItem( const QTreeWidgetItem *item ) const;

    void showBanner( const QString &message );
    void hideBanner();
    void updateFooter();
    void persist( const QString &key, const QVariant &value );
    QString defaultPathForSource( const QString &sourceId ) const;

    // --- widgets ---
    QComboBox   *sourceCombo_   = nullptr;
    QToolButton *filterButton_  = nullptr;
    QMenu       *filterMenu_    = nullptr;
    QToolButton *upButton_      = nullptr;
    QLineEdit   *pathEdit_      = nullptr;
    QLineEdit   *searchEdit_    = nullptr;
    QCheckBox   *recursiveBox_  = nullptr;
    SMediaBrowserTree *tree_    = nullptr;
    QLabel      *footer_        = nullptr;
    QFrame      *banner_        = nullptr;
    QLabel      *bannerLabel_   = nullptr;
    QPushButton *bannerRetry_   = nullptr;
    QTimer      *debounce_      = nullptr;

    // --- state ---
    QString  sourceId_;
    QString  path_;
    QString  needle_;
    int      categoryMask_ = 1;      // smedia::Audio
    bool     recursive_    = false;
    Mode     mode_         = Browse;
    bool     opened_       = false;  // has the current source been contacted?

    SMediaSource      *source_      = nullptr;   // not owned (the registry's)
    SLocalMediaSource *localSource_ = nullptr;   // owned by THIS panel
    // The gate-3 test provider, present ONLY under SMARAGD_MEDIA_TEST_SOURCE=1.
    // Also owned by this panel.
    SDelayedLocalSource *delayedSource_ = nullptr;

    // Supersession (inv. 2): the id of the ROOT request being displayed, and
    // the pending lazy expands. A batch tagged with anything else is dropped.
    int rootRequest_ = 0;
    QHash<int, QTreeWidgetItem *> expandRequests_;

    int     truncated_ = 0;
    QString error_;
};

#endif // SMEDIABROWSERPANEL_H
