
#ifndef _SEXTERN_FILE_H_
#define _SEXTERN_FILE_H_

#include "sobject.h"

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
};

#endif
