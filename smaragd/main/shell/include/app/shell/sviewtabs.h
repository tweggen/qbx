#ifndef SVIEWTABS_H
#define SVIEWTABS_H

#include <QTabWidget>
#include <QList>
#include <QPointer>
#include <QString>

// QPointer<SObject> below needs the complete QObject-derived type.
#include "app/model/sobject.h"

/**
 * The central widget: one tab per open editor root (proposal 09 §2).
 *
 * Tab 0 is the MASTER and is never closeable. With only that tab open the
 * experience is the one that was there before this class existed -- which is
 * the point of landing it on its own, because replacing the central widget
 * touches every reach-through in SMainWindow and the whole qxa suite is the
 * gate for "nothing changed".
 *
 * An entry is a (root SObject, editor QWidget) pair. The editor comes from
 * `root->getDetailEditWidget()`, so this class names no view type and needs no
 * change when a root of a different KIND grows its own editor (§1's Tracker
 * note). Ownership of the widget passes to the QTabWidget.
 *
 * DEDUP IS BY ROOT IDENTITY, not by name: one SObject gets at most one tab, so
 * the same arrangement reached two ways focuses the existing view instead of
 * spawning a second editor that would fight it over the same model.
 *
 * LIFETIME IS THE HAZARD (§8), and it is wired from the first commit rather
 * than later: a root can die under an open tab -- deleted, or undone back out
 * of existence -- and a tab left holding a dangling SObject* would crash on the
 * next repaint. Every entry watches its root's QObject::destroyed and closes
 * itself. The root is held as a QPointer for the same reason.
 */
class SViewTabs : public QTabWidget {
    Q_OBJECT

public:
    explicit SViewTabs( QWidget *parent = nullptr );
    ~SViewTabs() override;

    /**
     * Install the master editor as tab 0. Called once per project; replaces any
     * previous contents (a project swap tears every tab down with it).
     */
    void setMasterEditor( SObject *masterRoot, QWidget *editor,
                          const QString &label = QString() );

    /** The master tab's editor, or null before setMasterEditor(). */
    QWidget *masterEditor() const;

    /** The editor of the tab the user is looking at (the master when alone). */
    QWidget *activeEditor() const;

    /** The root the active tab edits, or null. */
    SObject *activeRoot() const;

    /**
     * Focus the tab for `root`, opening one if it has none. Returns the editor,
     * or null when the root vends no editor. `label` names the tab; the root's
     * own name is used when it is empty.
     */
    QWidget *openFor( SObject *root, const QString &label = QString() );

    /** Close the tab for `root`, if any. The MODEL is untouched: closing a tab
     *  destroys a view, never the object it was looking at. The master tab
     *  cannot be closed and the call is ignored for it. */
    void closeFor( SObject *root );

    /** Tab labels in order, master first. For assert-tab-set. */
    QStringList tabNames() const;

    /** The active tab's label. */
    QString activeTabName() const;

    /** Tear every tab down, master included (project close/swap). */
    void clearAll();

signals:
    /** The user switched tabs, or a tab closed and another became current.
     *  Carries the newly active root (null when nothing is open). */
    void activeRootChanged( SObject *root );

private slots:
    void onTabCloseRequested( int index );
    void onCurrentChanged( int index );
    void onRootDestroyed( QObject *obj );

private:
    struct Entry {
        QPointer<SObject> root;     // QPointer: a root can die under its tab
        QWidget          *editor = nullptr;   // owned by the QTabWidget
    };

    int indexOfRoot( const SObject *root ) const;
    void styleMasterTab();

    QList<Entry> entries_;          // parallel to tab indices, master at 0
};

#endif  // SVIEWTABS_H
