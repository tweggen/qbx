
#ifndef _SEXTERN_FILE_H_
#define _SEXTERN_FILE_H_

#include "app/model/sobject.h"

class SProject;

/**
 * Interface for extern file objects. They act as interfaces betwee the file system
 * and the actual code.
 *
 * Thread affinity: MIXED (depends on implementation)
 * Implementations (SPlainWave, etc.) may be accessed from multiple threads:
 * - UI thread: for preview rendering, serialization, property access
 * - Audio thread: for sample data playback via getRootComponent()
 *
 * NOTE: Subclasses must ensure thread-safe access to shared resources like file handles.
 */
class SExternFile
    : public SObject
{
    Q_OBJECT
public:
    SExternFile( SProject *project );
    virtual ~SExternFile() {}

    virtual QString getFileName() const = 0;

    /**
     * Point this object at a DIFFERENT PATH HOLDING THE SAME BYTES — what
     * "Collect external media" does when it copies a sample into the project's
     * own folder. Only the reference moves: the resident sample data, its
     * content hash and every sidecar keyed on that hash stay exactly as they
     * are, because a copy is not a different sample.
     *
     * Callers go through SProject::relocateExternFile(), which also rekeys the
     * project's by-path dictionary — calling this directly leaves the project
     * indexing the object under its old name.
     *
     * Returns false by default: a subclass that cannot be re-pointed simply
     * does not participate, and the collect pass reports it as skipped rather
     * than silently doing nothing.
     */
    virtual bool relocateTo( const QString & ) { return false; }

};

#endif
