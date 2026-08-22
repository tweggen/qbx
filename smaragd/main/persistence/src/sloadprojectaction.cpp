#include "app/persistence/sloadprojectaction.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/persistence/sprojectloader.h"
#include <QDomElement>

SLoadProjectAction::SLoadProjectAction(const QString &path)
    : path_(path)
{
}

SApplyResult SLoadProjectAction::apply(SProject *project)
{
    if (!project || path_.isEmpty()) {
        return {false, nullptr};
    }

    SProjectLoader loader(*project, path_);
    if (!loader.wasLoaded()) {
        return {false, nullptr};
    }

    // Anchor for the external file references inside the document: they are
    // stored relative to the project file (SFilePathRef), so this has to be set
    // BEFORE createObjects() resolves the first <SPlainWave filename='...'>.
    project->setProjectFilePath(path_);

    // Open the load window. Two things hang off it: a sample that will not load
    // records itself as a MISSING placeholder instead of raising its own modal
    // dialog (SProject::missingFiles, SPlainWave::setMissingWave), and the set
    // is cleared here so what the caller reads afterwards describes THIS load.
    project->beginLoad();

    // Suppress invalidation during project load to avoid deadlock.
    // All captures are empty on construction anyway, and worker threads
    // would race with the UI thread still deserializing objects.
    // When enableInvalidation() is called, worker threads will begin
    // recomputing all loaded cuts.
    // A load REPLACES the project, but it loads INTO this same SProject
    // object, so anything held in a by-name registry rather than in the object
    // tree has to be dropped explicitly or it is inherited by the document
    // being opened (proposal 09 M1).
    project->clearArrangements();

    project->disableInvalidation();

    if (loader.createObjects(*project) != 0) {
        // A failed load leaves a HALF-BUILT object graph that the caller is
        // about to discard (SMainWindow::openProjectFile marks it partial and
        // deleteLater()s it). Resuming background revalidation on it first
        // handed the worker pool raw pointers into objects that ~QObject was
        // about to free — the observed symptom was a crash on the SECOND open
        // attempt, with the first attempt's last log line a preview recompute.
        // pauseRevalidation() blocks until in-flight jobs drain, so quiesce
        // BEFORE balancing the suppression counter.
        project->pauseRevalidation();
        project->enableInvalidation();  // keep the counter balanced even on error
        project->endLoad();
        return {false, nullptr};
    }

    // Loading complete: re-enable invalidation and trigger revalidation pass.
    project->endLoad();
    project->enableInvalidation();

    // Not undoable: loading replaces the whole project; callers manage the swap.
    return {true, nullptr};
}

void SLoadProjectAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("path", path_);
}

bool SLoadProjectAction::readXml(const QDomElement &elem, int /*version*/)
{
    path_ = elem.attribute("path", "");
    return true;
}

static const bool s_reg_loadproject = (
    SActionRegistry::instance().registerType(
        QStringLiteral("load-project"),
        []{ return new SLoadProjectAction; }
    ), true
);
