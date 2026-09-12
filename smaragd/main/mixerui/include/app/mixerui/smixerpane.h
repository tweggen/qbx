#ifndef SMIXERPANE_H
#define SMIXERPANE_H

#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

#include "app/objects/mixer/slaneorder.h"
#include "tw/core/twtypes.h"

class QHBoxLayout;
class QScrollArea;

class SMixerStrip;
class SObject;
class SStdMixer;
class STrack;

/**
 * THE MIXER PANE (proposal 48 M1) — one horizontal dock, one channel strip
 * per track lane, and nothing else.
 *
 * Read `main/mixerui/CONTRACT.md` before changing anything here. The four
 * rules most easily broken:
 *
 *  - the lane list is `slaneorder::flattenTrackLanes()` and never a local
 *    walk (M0 / D1);
 *  - the pane IGNORES fold and honours hidden, EXCEPT that it always shows
 *    the master lane (D2 / D6a) — the one place the two mounts deliberately
 *    disagree about a model flag;
 *  - it follows `SViewTabs`' ACTIVE ROOT, so what it shows and what
 *    `stimeline::submitActive` stamps are the same arrangement (D12);
 *  - it joins the project-close detach seam, or it dereferences freed tracks
 *    on the next 33 ms meter tick (D13).
 */
class SMixerPane : public QWidget
{
    Q_OBJECT

public:
    /// THE PANE'S OWN WALK OPTIONS, as a value so they can be ASSERTED.
    ///
    /// `rebuildStrips()` and `rebuildIfStructureChanged()` are the only
    /// callers, and `laneorder_test` asserts what this returns — which is the
    /// only way the pane's CHOICE is gated at all. It cannot be gated from a
    /// `.qxa`: `collapse-track` drives the arranger's own
    /// `toggleTrackCollapsed()`, and a `--test-case` run has no bound
    /// arranger, so the fold flag cannot be set from a script (measured:
    /// `assert-lane-view collapsed=` reads false straight after the verb). A
    /// qxa case that collapsed a folder and asserted the strip count was
    /// therefore VACUOUS — it passed with `Fold::Honour` too, which is what
    /// M2's sabotage pass caught.
    static slaneorder::Options walkOptions();

    explicit SMixerPane( QWidget *parent = nullptr );
    ~SMixerPane() override;

    /// Point the pane at an arrangement root. Null clears it. Called from the
    /// shell on `SViewTabs::activeRootChanged` and on project open.
    void setRoot( SObject *root, const QString &rootName = QString() );
    QString rootName() const { return rootName_; }

    /// Drop every reference into the project. MUST be called from
    /// `SMainWindow::destroyDocksToolbars()` BEFORE the project is deleted.
    void detachProject();

    /// Rebuild the strip list from the model. Cheap enough to call on any
    /// STRUCTURE change (tracks added / removed / reordered / hidden) and
    /// deliberately NOT called for per-track signals — a rebuild deletes the
    /// fader under the hand mid-drag (CONTRACT inv. 7).
    void rebuildStrips();

    /// Rebuild ONLY when the lane list actually differs (CONTRACT inv. 7).
    /// The signal that reaches this fires after every action, and a blanket
    /// rebuild destroys each strip's meter ballistics and probe window — and
    /// would delete a fader under the hand mid-drag.
    void rebuildIfStructureChanged();

    int          stripCount() const { return strips_.size(); }
    SMixerStrip *stripAt( int i ) const;
    SMixerStrip *stripForTrackNamed( const QString &name ) const;

    QString describe() const;

    /// ONE METER TICK for the whole pane (proposal 48 M3).
    ///
    /// **THE DOCK GATE IS ANSWERED HERE, BEFORE THE PER-STRIP WALK** — CONTRACT
    /// inv. 5's "a hidden dock does no work, not even the model walk". A gate
    /// inside each strip has already paid for the walk by the time it runs.
    /// Returns how many strips did probe work, which is what `tickWork=` in
    /// `describe()` accumulates and what AC3.5 asserts against.
    int onMeterTick( offset_t pos, qint64 nowMs, bool live );

    /// Total strips-worked since construction — AC3.5's counter. Asserted, never
    /// timed: a timing assertion here would measure the box.
    qint64 tickWork() const { return tickWork_; }

    /// The four pane-wide section toggles (M2 stores them; M1 wires them).
    void setSectionsVisible( bool inserts, bool sends, bool meter, bool fader );

    /// The stored per-user section mask: 1 inserts | 2 sends | 4 meter |
    /// 8 fader (`SOpt::MixerSections`). NOT undoable — a preference is not an
    /// edit to the arrangement.
    static int  sectionsMask();
    static void setSectionsMask( int mask );
    /// Re-read the stored mask and apply it to every strip.
    void applyStoredSections();

private:
    void clearStrips_();

    QScrollArea *scroll_     = nullptr;  ///< the SCROLLING strip row
    QWidget     *stripHost_  = nullptr;
    QHBoxLayout *stripLayout_ = nullptr;
    QWidget     *masterHost_ = nullptr;  ///< the PINNED master column (D6)
    QHBoxLayout *masterLayout_ = nullptr;

    QPointer<SStdMixer>   mixer_;
    QString               rootName_;
    QList<QMetaObject::Connection> structureConns_;
    QList<SMixerStrip *>  strips_;
    SMixerStrip          *masterStrip_ = nullptr;

    bool showInserts_ = true;
    bool showSends_   = true;
    bool showMeter_   = true;
    bool showFader_   = true;
    qint64 tickWork_  = 0;
};

#endif // SMIXERPANE_H
