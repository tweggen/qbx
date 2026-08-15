#include "app/testkit/stestfilepath.h"
#include "app/model/sproject.h"

#include <QDir>
#include <QFileInfo>

QString resolveTestFilePath(const QString &filename, const QString &outputDir,
                            const SProject *project, QStringList *tried)
{
    if (filename.isEmpty()) {
        return filename;
    }

    if (QFileInfo(filename).isAbsolute()) {
        if (tried) tried->append(filename);
        return filename;
    }

    QStringList candidates;
    if (!outputDir.isEmpty()) {
        candidates.append(QDir::cleanPath(QDir(outputDir).filePath(filename)));
    }
    if (project && !project->sampleBaseDir().isEmpty()) {
        candidates.append(
            QDir::cleanPath(QDir(project->sampleBaseDir()).filePath(filename)));
    }
    candidates.append(QDir::cleanPath(QDir::current().filePath(filename)));

    if (tried) *tried = candidates;

    for (const QString &c : candidates) {
        if (QFileInfo::exists(c)) {
            return c;
        }
    }

    // Nothing exists. Hand back the output-dir spelling so the caller's error
    // reads the way it always did for a render that was never written.
    return candidates.first();
}
