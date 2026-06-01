#ifndef SACTIONHISTORY_H
#define SACTIONHISTORY_H

#include <QObject>
#include <QList>
#include <QMutex>

class SProject;
class SAction;
class SActionQueue;
class QUndoStack;

// Undo/redo bridge: in-flight action tracking + QUndoStack integration.
// Phase 1: synchronous drain on GUI thread.
// Phase 2: drain on engine thread between audio callbacks.
class SActionHistory : public QObject {
    Q_OBJECT

public:
    explicit SActionHistory(QObject *parent = nullptr);
    ~SActionHistory();

    // Submit a forward action from the GUI. Enqueues and drains synchronously (Phase 1).
    void submit(SAction *forward);

    // Undo the most recent action: try cancel if in-flight, else pop QUndoStack.
    void undo();

    // Redo the most recent undone action.
    void redo();

    // Access to the undo stack (for binding to UI, QUndoView, etc.).
    QUndoStack *undoStack() const;

private:
    // Internal: drain the queue synchronously, apply each action, populate undo stack.
    void drain_();

    // Called when an action is successfully applied.
    // Removes from in-flight, pushes to QUndoStack.
    void onApplied_(quint64 id, SAction *inverse);

    // Called when an action fails to apply.
    void onRejected_(quint64 id, const QString &reason);

    struct InFlight {
        quint64   id;
        SAction  *forward;
    };

    QList<InFlight> inFlight_;     // GUI-thread only; FIFO order
    SActionQueue   *queue_;
    QUndoStack     *undoStack_;
};

#endif // SACTIONHISTORY_H
