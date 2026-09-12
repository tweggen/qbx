#ifndef SMIXERPANE_H
#define SMIXERPANE_H

#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

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

    int          stripCount() const { return strips_.size(); }
    SMixerStrip *stripAt( int i ) const;
    SMixerStrip *stripForTrackNamed( const QString &name ) const;

    /// `root=…|strips=N|master=0|1|names=a,b,c`
    QString describe() const;

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
};

#endif // SMIXERPANE_H
