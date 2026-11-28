#ifndef _SUNUSEDCLIPS_H_
#define _SUNUSEDCLIPS_H_

#include <QList>
#include <QString>

#include "tw/core/twtypes.h"

class SObject;
class SProject;

/**
 * THE UNUSED CLIPS: the loaded orphans the purge command may release
 * (QBX-319, model/CONTRACT.md "Loaded orphans").
 *
 * An entry is a clip `SProject` PINNED at load (`pinLoadedOrphan`) that
 * nothing places NOW. Nothing else is listed. A clip made in this session that
 * lost its last placement belongs to the undo history that holds it, not to
 * this list, and is never offered (author, 2026-11-27).
 *
 * ONE FUNCTION, THREE READERS. The Edit menu greys "Purge unused clips..." on
 * `list().isEmpty()`, the confirmation shows `label()` for each row, and
 * `purge-unused-clips rows=` addresses an entry by its INDEX in `list()`. If
 * any of the three computed its own list, a row the user ticked could
 * release a different clip. `assert-unused-clips` reads `describe()`, which is
 * built from the same `list()`.
 *
 * THE ORDER IS DETERMINISTIC, because an index is an address. Sorted by the
 * content's file name, then the window's start, then its length, then the
 * clip's position in the file it was loaded from. The pin order alone came
 * from iterating a QHash, which changes from run to run.
 */
namespace sunusedclips {

struct Entry {
    SObject *clip = nullptr;
    /// The window content's file (SExternFile::getFileName()). Empty for
    /// content that is not a file (an event sequence).
    QString  contentFile;
    /// The content's class name when it is not a file, for the label.
    QString  contentKind;
    /// SClipWindow::startOffset() / duration(): timeline frames.
    offset_t start  = 0;
    length_t length = 0;
    /// The clip's position among the loaded orphans, in file order.
    int      filePos = 0;

    /// One line a user can recognise the clip by: the content's base name,
    /// where in it the window starts, and how long it is, in seconds at the
    /// project rate.
    QString label( int sampleRate ) const;
};

/// The unused loaded clips, sorted. See the namespace note.
QList<Entry> list( SProject *project );

/// "count=N|0:<label>|1:<label>..." built from list(). For assert-unused-clips.
QString describe( SProject *project );

} // namespace sunusedclips

#endif // _SUNUSEDCLIPS_H_
