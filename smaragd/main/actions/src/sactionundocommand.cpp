#include "app/actions/sactionundocommand.h"
#include "app/actions/saction.h"
#include "app/actions/sactionhistory.h"

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
    // Undo means: apply the inverse action without adding to history (to avoid recursion).
    // The inverse action is reusable; it's owned by this undo command and will be
    // deleted in the destructor. Don't null out the pointer.
    if (history_ && inverse_) {
        history_->submit(inverse_, true);  // skipHistory = true
    }
}

void SActionUndoCommand::redo()
{
    // The first redo() is called by Qt's QUndoStack::push() after the action
    // has already been applied in drain_(). Skip it to avoid double-applying.
    // Subsequent redo() calls (from user clicking redo) should re-apply.
    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }

    // Redo means: apply the forward action without adding to history (to avoid recursion).
    // The forward action is reusable; it's owned by this undo command and will be
    // deleted in the destructor. Don't null out the pointer.
    if (history_ && forward_) {
        history_->submit(forward_, true);  // skipHistory = true
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

    // NEVER MERGE ACROSS ROOTS (proposal 09 D21). A mergeKey is built from a
    // PATH, and index {0} exists in every root -- so two faders at the same
    // index in two different arrangements can produce the same key. The
    // surviving command carries ONE captured root, so the merge would fold two
    // edits in different trees into one undo entry that then restores the wrong
    // one. Every key that names a path is root-qualified individually, and this
    // is the backstop for the one that is added later and forgets: a guard here
    // cannot be forgotten, a key can.
    if (forward_->pathRoot() != cmd->forward_->pathRoot()) {
        return false;
    }

    return forward_->mergeWith(cmd->forward_);
}
