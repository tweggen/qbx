#ifndef _SRECORDINGFILES_H_
#define _SRECORDINGFILES_H_

#include <QList>
#include <QString>

class SProject;

/**
 * THE RECORDER'S CAPTURE FILES, as a NAME CONVENTION and a SCAN (plan 53 §6.3,
 * P6).
 *
 * A capture file is written per armed track per press, named by wall clock:
 * `<stamp>_track<n>.wav`, where the stamp is `yyyyMMdd_HHmmss_zzz`. That
 * convention is the only thing connecting a file on disk to "this came out of
 * the recorder", and two separate places need to agree about it: the recorder
 * that WRITES the names, and the Unused recordings scan that RECOGNISES them.
 *
 * SO IT IS DEFINED ONCE, HERE, and `SAudioRecorder` composes its filenames
 * through it. Before this the pattern was a `QStringLiteral` inside
 * `SAudioRecorder::start()` and nothing else knew it; a scan with its own copy
 * of the format string would have been a second definition of the same fact,
 * and the failure when they drifted would have been silent -- orphan files
 * simply not reported.
 *
 * WHY `model` AND NOT `shell`. The scan has two consumers in slices that
 * cannot see each other: `cleanup-takes` (objects/cut) removes what it finds,
 * and the Unused recordings dock presents the same list without removing. The
 * lowest slice both reach is this one, and the scan needs nothing above it --
 * a project, its extern files, and a directory listing.
 */
namespace srecfiles {

/** The capture file name for one armed track of one press. No directory. */
QString captureName( const QString &stamp, int trackIndex );

/**
 * Does this BASE NAME look like a capture this recorder wrote?
 *
 * Anchored and strict: `20261014_190949_902_track0.wav` yes, a user's
 * `guitar.wav` no, and `20261014_190949_902_track0.wav.bak` no. A loose test
 * here would offer to bin a file the user named themselves, which is the one
 * mistake this whole feature may not make.
 */
bool isCaptureName( const QString &baseName );

/** Where a project's captures are written (its own directory). */
QString captureDirOf( const SProject *project );

/** One recording nothing in the project uses, and why it is listed. */
struct Orphan {
    QString path;   ///< absolute, as found
    QString why;    ///< one line, already user-facing
};

/**
 * CAPTURE FILES AND EXTERN FILES THIS PROJECT DOES NOT USE.
 *
 * READ FROM THE DISK, compared against what is PLACED (QBX-315; convention
 * unused-recordings-independent-of-lifetime). An undone take's WAV is listed
 * whether or not its model objects are still alive: once nothing places them
 * they are garbage, and whether a DeferredDelete has reached them yet is
 * timing, so the list may not be asked of them. Two sources:
 *
 *  - the DISK: a file that passes isCaptureName(), exists, and shares its name
 *    with no extern file a clip places or an asset registers. Candidates are
 *    the project's own directory (a saved project) and the files this
 *    document's recorder wrote (SProject::captureFiles -- the only way to find
 *    an UNTITLED project's captures without sweeping a shared directory such
 *    as Documents, which holds other projects' recordings). A press whose take
 *    was undone, refused by every armed lane, or a project saved without it;
 *  - the MODEL: an `SExternFile` the project still holds with NO PLACEMENTS,
 *    which is not a registered asset, and which the disk half did not already
 *    list. That catches what the disk cannot see: a user's own sample whose
 *    clips are all gone, or a recording whose file was moved away. A capture
 *    is never listed twice, so its entry reads the same either way.
 *
 * A PLACED RECORDING IS NEVER LISTED: being placed is the one question asked
 * of the objects, and a placed file's objects are alive because a clip holds
 * them.
 *
 * READ-ONLY. It removes nothing and touches nothing -- which is what lets the
 * dock and the clean-up share it, and what makes the COUNT gateable while the
 * deletion is not.
 */
QList<Orphan> scanUnused( SProject *project );

} // namespace srecfiles

#endif // _SRECORDINGFILES_H_
