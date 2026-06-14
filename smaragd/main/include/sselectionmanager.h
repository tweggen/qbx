#ifndef SSELECTIONMANAGER_H
#define SSELECTIONMANAGER_H

#include <QList>
#include "slink.h"

class SProject;

typedef QList<SLink*> SSelectionList;

/**
 * Helper to convert between live SLink pointers and index paths for
 * serialization and undo/redo of selection actions.
 */
class SSelectionManager {
public:
    SSelectionManager() = default;

    // Convert live SLink pointers → index paths (each path from root SObject)
    QList<QList<int>> linksToPaths(const SSelectionList &links, SProject *project) const;

    // Convert index paths → live SLink pointers (null in list if path invalid)
    SSelectionList pathsToLinks(const QList<QList<int>> &paths, SProject *project) const;

    // Validate: does a path point to a live, valid SLink?
    bool isPathValid(const QList<int> &path, SProject *project) const;
};

#endif // SSELECTIONMANAGER_H
