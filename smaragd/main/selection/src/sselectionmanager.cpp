#include "app/selection/sselectionmanager.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/sobjectpath.h"

QList<QList<int>> SSelectionManager::linksToPaths(const SSelectionList &links, SProject *project,
                                              const QString &root) const
{
    QList<QList<int>> paths;
    if (!project) return paths;

    SObject *rootObj = splacements::rootNamed( project, root );
    if (!rootObj) return paths;

    for (SLink *link : links) {
        if (!link) continue;
        QList<int> path = strackpath::pathOf(rootObj, &link->getSObject());
        paths.append(path);
    }
    return paths;
}

SSelectionList SSelectionManager::pathsToLinks(const QList<QList<int>> &paths, SProject *project,
                                           const QString &root) const
{
    SSelectionList links;
    if (!project) return links;

    SObject *rootObj = splacements::rootNamed( project, root );
    if (!rootObj) return links;

    for (const QList<int> &path : paths) {
        SObject *obj = strackpath::resolveByPath(rootObj, path);
        if (!obj) {
            links.append(nullptr);  // Keep nulls in place to preserve order
            continue;
        }

        // obj is an SObject; we need the SLink that holds it
        // The SLink is the parent container's child (by the same path-1)
        if (path.isEmpty()) {
            // Root itself — can't select root
            links.append(nullptr);
            continue;
        }

        // Get parent and find the link to obj
        QList<int> parentPath = path.mid(0, path.size() - 1);
        SObject *parent = strackpath::resolveByPath(rootObj, parentPath);
        if (!parent) {
            links.append(nullptr);
            continue;
        }

        int childIdx = path.last();
        SLink *link = parent->childAt(childIdx);
        links.append(link);
    }
    return links;
}

bool SSelectionManager::isPathValid(const QList<int> &path, SProject *project,
                                  const QString &root) const
{
    if (!project) return false;
    SObject *rootObj = splacements::rootNamed( project, root );
    if (!rootObj) return false;
    return strackpath::resolveByPath(rootObj, path) != nullptr;
}
