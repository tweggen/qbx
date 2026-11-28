#ifndef SPURGEUNUSEDCLIPSACTION_H
#define SPURGEUNUSEDCLIPSACTION_H

#include "app/actions/saction.h"

#include <QList>
#include <QStringList>

/**
 * `purge-unused-clips` (QBX-319): RELEASE LOADED ORPHANS, the one verb that
 * may (model/CONTRACT.md "Loaded orphans").
 *
 * Since QBX-314 the project pins every clip a file describes and nothing in
 * it places, so such a clip is written by every save and pinned again by
 * every load: it stays forever. The author accepted that on the condition
 * that this command exists. It is the ONLY release of a single pin; the other
 * release, `SProject::clearLoadedOrphans()`, drops all of them when a load
 * replaces the document.
 *
 * ADDRESSING. `rows="0;2"` names entries by their index in
 * `sunusedclips::list()`, the sorted list the Edit menu's confirmation shows.
 * `all="1"` releases every entry. Exactly one of the two must be given.
 *
 * REFUSED, with nothing released, when the list is empty, when neither or
 * both of `rows` and `all` are given, and when a row is out of range, repeated
 * or not a number. A partial purge of a mistyped list is the one outcome this
 * verb must not have: the confirmation asked about specific clips.
 *
 * NOT UNDOABLE (author, 2026-11-27). The obvious inverse is an undo PIN that
 * keeps the released clip alive. But `SProject::serialize()` writes every
 * object still alive under the project, so that pin would put the clip into
 * the next save, and the next load would pin it again: the purge would undo
 * itself across a save. Instead the user confirms first, as with
 * `cleanup-takes files="1"`. It still MARKS THE PROJECT MODIFIED
 * (`marksProjectModified()`): the next save writes less than the last one did.
 *
 * WHAT ELSE DIES. A released clip's last reference is gone, so it is deleted
 * on the next turn of the event loop. Whatever only it windowed goes with it:
 * the sample's extern file, or a missing-media placeholder. That file then
 * shows in "Unused recordings..." if it is a capture. A .qxa must run
 * `drain-pending-deletes` before it saves, because that turn of the loop is
 * what a scripted run does not have.
 */
class SPurgeUnusedClipsAction : public SAction {
public:
    SPurgeUnusedClipsAction() = default;
    /// `rows` in `sunusedclips::list()` order; an empty list with `all`
    /// false is refused at apply time.
    SPurgeUnusedClipsAction( const QList<int> &rows, bool all );

    QString name() const override
    { return QStringLiteral( "purge-unused-clips" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "rows" ), QStringLiteral( "all" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    bool marksProjectModified() const override { return true; }

    /// Where apply() writes how many clips it released. READ IT FROM HERE,
    /// never from the action after a submit: the history deletes an action
    /// that applied with no inverse, which this one always is (QBX-276's
    /// lesson in scleanuptakesaction.h).
    void reportTo( int *released ) { report_ = released; }

private:
    QString rowsText_;          // as written: ';'-separated indices
    bool    all_ = false;
    int    *report_ = nullptr;  // borrowed; see reportTo()
};

#endif // SPURGEUNUSEDCLIPSACTION_H
