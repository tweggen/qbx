#include "app/actions/sactionqueue.h"
#include "app/actions/saction.h"

quint64 SActionQueue::enqueue(SAction *action)
{
    QMutexLocker lock(&mtx_);

    // Check if the tail action can absorb this one via mergeWith.
    if (!queue_.isEmpty()) {
        Entry &tail = queue_.back();
        if (tail.action->mergeKey() == action->mergeKey() &&
            tail.action->mergeWith(action)) {
            // Merged: the newcomer is absorbed, tail's id stays valid for the caller.
            delete action;
            return tail.id;
        }
    }

    // Not merged: add as new entry.
    quint64 id = nextId_++;
    queue_.append(Entry{id, action});
    return id;
}

SAction *SActionQueue::dequeue(quint64 *outId)
{
    QMutexLocker lock(&mtx_);

    if (queue_.isEmpty()) {
        return nullptr;
    }

    Entry e = queue_.front();
    queue_.pop_front();

    if (outId) {
        *outId = e.id;
    }

    return e.action;
}

bool SActionQueue::tryCancel(quint64 id)
{
    QMutexLocker lock(&mtx_);

    for (int i = 0; i < queue_.count(); ++i) {
        if (queue_[i].id == id) {
            SAction *action = queue_[i].action;
            queue_.removeAt(i);
            delete action;
            return true;
        }
    }

    return false;
}

QList<SAction*> SActionQueue::snapshotPending() const
{
    QMutexLocker lock(&mtx_);

    QList<SAction*> result;
    for (const Entry &e : queue_) {
        // We'd need to deep-copy each action for true serialization.
        // For Phase 1, we skip this.
        // TODO: Phase 2 - implement action cloning for serialization.
    }
    return result;
}
