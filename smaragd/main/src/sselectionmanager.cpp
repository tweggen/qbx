#include "sselectionmanager.h"
#include "sproject.h"
#include "actions/strackpath.h"

QList<QList<int>> SSelectionManager::linksToPaths(const SSelectionList &links, SProject *project) const
{
    QList<QList<int>> paths;
    if (!project) return paths;

    SObject *root = project->getRootComponent();
    if (!root) return paths;

    for (SLink *link : links) {
        if (!link) continue;
        QList<int> path = strackpath::pathOf(root, &link->getSObject());
        paths.append(path);
    }
    return paths;
}

SSelectionList SSelectionManager::pathsToLinks(const QList<QList<int>> &paths, SProject *project) const
{
    SSelectionList links;
    if (!project) return links;

    SObject *root = project->getRootComponent();
    if (!root) return links;

    for (const QList<int> &path : paths) {
        SObject *obj = strackpath::resolveByPath(root, path);
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
        SObject *parent = strackpath::resolveByPath(root, parentPath);
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

bool SSelectionManager::isPathValid(const QList<int> &path, SProject *project) const
{
    if (!project) return false;
    SObject *root = project->getRootComponent();
    if (!root) return false;
    return strackpath::resolveByPath(root, path) != nullptr;
}
