
#ifndef _SEXTERN_FILE_H_
#define _SEXTERN_FILE_H_

#include "sobject.h"

class SProject;

/**
 * Interface for extern file objects. They act as interfaces betwee the file system
 * and the actual code.
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
