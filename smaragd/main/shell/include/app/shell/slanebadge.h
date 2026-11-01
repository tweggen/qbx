#ifndef SLANEBADGE_H
#define SLANEBADGE_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QToolButton>

class SObject;
class SProject;

/**
 * A STATUS-BAR BADGE FOR A FLAG THAT LANES CARRY ANYWHERE IN THE PROJECT.
 *
 * QBX-257 built the first one for solo; this is its mechanism, lifted out when
 * the author asked for "the same display for at least one track is armed".
 *
 * WHY ONE COPY. The badge's hard part is not the word on the button, it is
 * naming WHERE: `splacements::specOf()` and `splacements::rootHolding()` come
 * out of one walk in one order by construction, so the spelling and the root
 * cannot describe different places — which is exactly what deriving the root
 * by re-resolving the qualifier would risk, and exactly the mistake the
 * QBX-231 / 233 / 243 / 244 / 245 sweep kept finding. ssolorules.h's header
 * records what happened to the one rule that was written out three times: all
 * three copies were wrong in the same way.
 *
 * WHAT A SUBCLASS OWNS: the lane set (which flag, and the walk that finds it),
 * the word, its tooltip's sentences, and the PREFIX of its describe() fields.
 * Everything else — counting, root naming, the on-screen question, the hiding,
 * the palette, the describe() shape — is here.
 *
 * IT IS A BUTTON, NOT A LAMP. An indicator that only informs leaves the user
 * to hunt for the lane, and the hunt IS the problem; the subclass says what
 * clicking does and `SMainWindow` owns the command (one QAction, shared with a
 * menu item, so the badge and the menu cannot drift about being enabled).
 *
 * NOT COLOUR ALONE. The word carries the state; the colour is decoration, and
 * it comes from the palette's Highlight role rather than a hex literal, so it
 * follows the light and the dark theme instead of being legible in one.
 */
class SLaneBadge : public QToolButton {
    Q_OBJECT

public:
    /** True while at least one lane in the project carries the flag. */
    bool inForce() const { return lanes_ > 0; }

protected:
    /** `objectName` is the widget's own name, which the layout gates use. */
    explicit SLaneBadge( const QString &objectName, QWidget *parent = nullptr );

    /** What the subclass has to say; every string is its own, and every one of
     *  them is user-visible and translatable there. */
    struct Words {
        QString word;        ///< what is PAINTED: `SOLO`, `ARM`
        QString what;        ///< tooltip line 1: what the state makes true
        QString listLabel;   ///< tooltip line 2, with one `%1` for the paths
        QString click;       ///< tooltip line 3: what a click does
    };

    /**
     * Recompute everything from the lane set the subclass found.
     *
     * `activeRoot` is the root the user is looking at
     * (`SViewTabs::activeRoot()`) and decides only whether the root is NAMED,
     * never whether the badge appears: a flag the user cannot see is the case
     * these widgets exist for. `masterLabel` is what the master's own tab is
     * called, so the badge names the master the way the tab bar does rather
     * than inventing a second word for it.
     */
    void refreshFrom( SProject *project, SObject *activeRoot,
                      const QString &masterLabel,
                      const QList<SObject *> &lanes, const Words &words );

    /** No project: nothing in force, nothing shown. */
    void clearState();

    /**
     * The first five fields of a badge's test face, `<p>` being the subclass's
     * prefix, in THIS order and never re-ordered — `assert-status-bar` matches
     * SUBSTRINGS, so a field moved between two others breaks every assertion
     * spanning the seam:
     *
     *   <p>Shown=<0|1>      !isHidden() — the WIDGET's own state, not our
     *                       intent, so a build that computes the label and
     *                       forgets to hide the badge cannot pass
     *   <p>Text=<text>      text(), i.e. exactly what is painted
     *   <p>Lanes=<n>        how many lanes carry the flag, whole project
     *   <p>Roots=<a;b>      the roots holding them, display labels, in root
     *                       order, deduped, ';'-joined (a name containing ';'
     *                       is written as is — this field is for gates)
     *   <p>OnScreen=<0|1>   every such lane is in the root on screen
     *
     * The subclass appends its own tail, so QBX-257's exact field order
     * (`soloScope` before `soloList`) is preserved.
     */
    QString describeFields( const QString &prefix ) const;

    /** The qualified paths the tooltip lists, space-joined: the subclass's
     *  `<p>List=` field. */
    QString specList() const;

    int         lanes_ = 0;
    QStringList roots_;
    QStringList specs_;
    bool        onScreen_ = true;

private:
    void apply_( const QString &text, const QString &tip );
};

#endif  // SLANEBADGE_H
