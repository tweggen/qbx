#ifndef SACTIONQUEUE_H
#define SACTIONQUEUE_H

#include <QMutex>
#include <QList>

class SAction;

// Thread-safe queue with merge-on-enqueue and cancel-before-drain.
// Designed for async drain but safe for synchronous use (Phase 1).
class SActionQueue {
public:
    // Enqueue an action. May merge into queue tail via mergeKey + mergeWith.
    // Returns an id usable with tryCancel.
    quint64 enqueue(SAction *action);

    // Dequeue the next action (only one at a time). Returns null when empty.
    // Outid receives the action's id.
    SAction *dequeue(quint64 *outId);

    // Try to cancel an action before the engine drains it.
    // Returns true if found and removed; false if already drained.
    bool tryCancel(quint64 id);

    // Snapshot of currently pending actions (for save). Caller owns the list.
    QList<SAction*> snapshotPending() const;

private:
    struct Entry {
        quint64   id;
        SAction  *action;
    };

    mutable QMutex mtx_;
    QList<Entry>   queue_;
    quint64        nextId_ = 1;
};

#endif // SACTIONQUEUE_H
