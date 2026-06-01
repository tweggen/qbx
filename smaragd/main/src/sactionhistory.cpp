#include "sactionhistory.h"
#include "sactionqueue.h"
#include "sactionundocommand.h"
#include "saction.h"
#include "sproject.h"
#include "sapplication.h"

#include <QUndoStack>

SActionHistory::SActionHistory(QObject *parent)
    : QObject(parent),
      queue_(new SActionQueue),
      undoStack_(new QUndoStack(this))
{
}

SActionHistory::~SActionHistory()
{
    delete queue_;
}

void SActionHistory::submit(SAction *forward)
{
    // Phase 1: synchronous drain on GUI thread.
    // Phase 2 would defer this to engine thread + async signal.

    quint64 id = queue_->enqueue(forward);
    drain_();
}

void SActionHistory::drain_()
{
    SProject *project = SApplication::app().getCurrentProject();
    if (!project) {
        // No active project; skip.
        return;
    }

    quint64 id;
    SAction *action;
    while ((action = queue_->dequeue(&id)) != nullptr) {
        SApplyResult result = action->apply(project);

        if (result.applied) {
            onApplied_(id, result.inverse);
        } else {
            onRejected_(id, "Apply failed");
        }

        delete action;
    }
}

void SActionHistory::onApplied_(quint64 id, SAction *inverse)
{
    // Remove from in-flight.
    for (int i = 0; i < inFlight_.count(); ++i) {
        if (inFlight_[i].id == id) {
            SAction *forward = inFlight_[i].forward;
            inFlight_.removeAt(i);

            // Push to QUndoStack as an SActionUndoCommand.
            if (inverse) {
                undoStack_->push(new SActionUndoCommand(forward, inverse, this));
            } else {
                // Non-undoable action: don't add to undo stack, but still clean up.
                delete forward;
            }
            return;
        }
    }
}

void SActionHistory::onRejected_(quint64 id, const QString &reason)
{
    // Remove from in-flight and log.
    for (int i = 0; i < inFlight_.count(); ++i) {
        if (inFlight_[i].id == id) {
            SAction *forward = inFlight_[i].forward;
            inFlight_.removeAt(i);

            // TODO: Log error and notify UI.
            // For Phase 1, just clean up.
            delete forward;
            return;
        }
    }
}

void SActionHistory::undo()
{
    // Two-tier undo: try to cancel in-flight first, else use QUndoStack.

    if (!inFlight_.isEmpty()) {
        // Most recent is at the end.
        InFlight &latest = inFlight_.back();
        if (queue_->tryCancel(latest.id)) {
            // Cancelled before engine drained it.
            delete latest.forward;
            inFlight_.pop_back();
            return;
        }
    }

    // Already applied; fall through to standard undo.
    undoStack_->undo();
}

void SActionHistory::redo()
{
    undoStack_->redo();
}

QUndoStack *SActionHistory::undoStack() const
{
    return undoStack_;
}
