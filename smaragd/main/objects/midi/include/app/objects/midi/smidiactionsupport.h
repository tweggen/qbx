#ifndef _SMIDIACTIONSUPPORT_H_
#define _SMIDIACTIONSUPPORT_H_

#include <QList>
#include <QString>
#include <vector>

#include "app/objects/midi/smidisequence.h"

class SLink;
class SMidiCut;
class SProject;

/**
 * The two things every MIDI verb needs and nothing else: resolve a clip path
 * to the cut + the sequence behind it, and turn a `<n .../>` / `<e .../>` child
 * list into model events. Kept out of the individual verbs so "what a clip
 * path means" has ONE spelling here, exactly as `splacements` gives the audio
 * verbs one.
 */
namespace smidiactions {

struct ClipRef {
    SLink         *link = nullptr;   // the placement (owns the timeline start)
    SMidiCut      *cut  = nullptr;   // the window (or a stack's addressed take)
    SMidiSequence *seq  = nullptr;   // the content

    bool valid() const { return link && cut && seq; }
};

/**
 * `clipPath` addresses a placement from the root mixer. A take stack resolves
 * through the generic take seam (`SObject::windowTakeAt`, -1 = active), so
 * this slice never names STakeStack - a stack is homogeneous by contentKind,
 * so a MIDI clip's stack yields MIDI takes or nothing.
 */
ClipRef resolveClip( SProject *project, const QList<int> &clipPath,
                     int take = -1 );

/** A `<n tick= dur= key= velocity= channel= releaseVelocity= m=/>` child. */
SEvent readNoteChild( const class QDomElement &el );
void writeNoteChild( class QDomElement &parent, const SEvent &e );

/** Serialize a whole table as `<e .../>` children (the universal inverse). */
void writeEventChildren( class QDomElement &parent,
                         const std::vector<SEvent> &events );
std::vector<SEvent> readEventChildren( const class QDomElement &parent );

/**
 * Is `e` the note addressed by (tick, key, channel)? channel < 0 is a
 * wildcard, which is what a script that does not care about channels wants.
 */
bool matchesNote( const SEvent &e, qint64 tick, int key, int channel );

}  // namespace smidiactions

#endif // _SMIDIACTIONSUPPORT_H_
