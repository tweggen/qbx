#ifndef SSELECTIONMANAGER_H
#define SSELECTIONMANAGER_H

#include <QList>
#include <QString>
#include "app/model/slink.h"
#include "app/model/sobject.h"

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
    QList<QList<int>> linksToPaths(const SSelectionList &links, SProject *project,
                                   const QString &root = QString()) const;

    // Convert index paths → live SLink pointers (null in list if path invalid)
    SSelectionList pathsToLinks(const QList<QList<int>> &paths, SProject *project,
                                const QString &root = QString()) const;

    // Validate: does a path point to a live, valid SLink?
    bool isPathValid(const QList<int> &path, SProject *project,
                     const QString &root = QString()) const;

    // AC-a3 (Ctrl-A): every CLIP's path under `root` — a lane's own children,
    // recursing into a nested lane (a folder track) rather than counting it as
    // a clip itself. Lane-ness is SObject::isPathContainer(), the same test
    // splacements.h uses, so this needs no concrete STrack/SStdMixer type.
    // Indices match childAt()/indexOfChild() exactly (every child counts,
    // container or not), so a path here resolves through the ordinary
    // SObjectPath machinery like any other.
    QList<QList<int>> allClipPaths(SObject *root) const;
};

#endif // SSELECTIONMANAGER_H
