
#ifndef _SFILEPATHREF_H_
#define _SFILEPATHREF_H_

#include <qstring.h>

/**
 * How a reference to an EXTERNAL file (a sample WAV, ...) is written into a
 * project file, and how it is read back.
 *
 * A project must stay openable after the whole folder holding it has been
 * moved, copied to another machine, or checked into a repository — so an
 * absolute path is the WORST possible encoding and is used only when nothing
 * else can express the reference. The three forms, in order of preference:
 *
 *   1. PROJECT-RELATIVE ("samples/kick.wav", "../shared/kick.wav")
 *      The default. Survives moving the project folder as a whole.
 *
 *   2. HOME-RELATIVE ("~/audio/library/kick.wav")
 *      Used when the relative path would have to climb all the way UP TO the
 *      home directory (i.e. the project and the file share no ancestor below
 *      home). Such a "../.." chain says nothing but "somewhere else in my
 *      home", and it breaks as soon as the project moves one level; anchoring
 *      at ~ survives that AND a different user name on another machine.
 *
 *   3. ABSOLUTE ("C:/Windows/Media/x.wav", "/usr/share/sounds/x.wav")
 *      Only when the climb goes BEYOND home to a filesystem root (or the two
 *      paths have no common root at all, e.g. different Windows drives).
 *      There is nothing portable left to say, so say it plainly.
 *
 * The "~" prefix is the marker for form 2 and is the only reserved spelling —
 * a stored path is home-relative iff it starts with "~/" (or is exactly "~").
 *
 * All functions are pure string/path arithmetic: they never touch the disk
 * (existence is the caller's business — see SPlainWave's load path, which
 * keeps a legacy fallback when the resolved file is not there).
 */
namespace SFilePathRef {

/**
 * Encode `filePath` for storage in the project file at `projectFilePath`.
 * A relative `filePath` is first resolved against the working directory.
 * An empty `projectFilePath` (an untitled project) leaves nothing to be
 * relative TO, so the absolute path is returned.
 */
QString toStored( const QString &filePath, const QString &projectFilePath );

/**
 * The inverse: turn a stored reference back into an absolute path, using the
 * project file's directory as the anchor. Handles all three forms. A relative
 * form with an empty `projectFilePath` is returned unchanged (the caller then
 * resolves it however it used to).
 */
QString fromStored( const QString &storedPath, const QString &projectFilePath );

} // namespace SFilePathRef

#endif
