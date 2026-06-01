#include "sactionundocommand.h"
#include "saction.h"
#include "sactionhistory.h"

SActionUndoCommand::SActionUndoCommand(SAction *forward, SAction *inverse, SActionHistory *h)
    : forward_(forward), inverse_(inverse), history_(h)
{
    setText(forward->name());
}

SActionUndoCommand::~SActionUndoCommand()
{
    // Ownership: forward and inverse are kept for redo/undo.
    // They're deleted when the UndoStack discards this command.
    delete forward_;
    delete inverse_;
}

void SActionUndoCommand::undo()
{
    // Undo means: submit the inverse action.
    // The inverse is the action that reverses the forward.
    if (history_ && inverse_) {
        history_->submit(inverse_);
        // After submit, inverse_ is owned by the newly created undo command.
        // We shouldn't delete it here.
        inverse_ = nullptr;
    }
}

void SActionUndoCommand::redo()
{
    // Redo means: submit the forward action again.
    if (history_ && forward_) {
        history_->submit(forward_);
        // After submit, forward_ is owned by the newly created undo command.
        forward_ = nullptr;
    }
}

int SActionUndoCommand::id() const
{
    // Merge id based on action mergeKey.
    QString key = forward_ ? forward_->mergeKey() : QString();
    if (key.isEmpty()) {
        return -1;  // No merge id = don't attempt QUndoStack::push merging
    }
    // Hash the key to an int.
    return qHash(key) & 0x7FFFFFFF;  // Positive int
}

bool SActionUndoCommand::mergeWith(const QUndoCommand *other)
{
    // Delegate to SAction mergeWith logic.
    // (This is less likely to be used in Phase 1 since we merge at enqueue time.)
    const SActionUndoCommand *cmd = dynamic_cast<const SActionUndoCommand *>(other);
    if (!cmd || !cmd->forward_) {
        return false;
    }

    if (!forward_) {
        return false;
    }

    return forward_->mergeWith(cmd->forward_);
}
