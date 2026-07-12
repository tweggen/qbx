#ifndef SACTIONUNDOCOMMAND_H
#define SACTIONUNDOCOMMAND_H

#include <QUndoCommand>

class SAction;
class SActionHistory;

// QUndoCommand wrapper around a forward+inverse action pair.
// Undo/redo both go back through SActionHistory::submit for consistency.
class SActionUndoCommand : public QUndoCommand {
public:
    SActionUndoCommand(SAction *forward, SAction *inverse, SActionHistory *h);
    ~SActionUndoCommand();

    void undo() override;
    void redo() override;

    // Merge id based on mergeKey (delegates to SAction).
    int id() const override;
    bool mergeWith(const QUndoCommand *other) override;

private:
    SAction         *forward_;   // ownership: kept for redo
    SAction         *inverse_;   // ownership: kept for undo
    SActionHistory  *history_;   // back-pointer for submit
    bool            firstRedo_ = true;  // track if redo() is being called for initial state
};

#endif // SACTIONUNDOCOMMAND_H
