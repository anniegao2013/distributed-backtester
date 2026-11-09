#pragma once

#include "protocol/protocol.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

// The unit stored in the coordinator's job queue. The index preserves the
// position of the job in the finalized experiment's request order; outcomes are
// written back to that slot.
struct QueuedJob {
    std::size_t index = 0;
    BacktestJobMessage message;
};

// Thread-safe FIFO queue used to distribute jobs across workers.
//
// The coordinator pushes every expanded job once, then closes the queue.
// Workers call pop() to take the next job in insertion order. push() appends to
// the back and pop() removes from the front, so distribution is FIFO even when
// several workers pop concurrently. A blocked pop() wakes when a job arrives or
// when close() is called.
class JobQueue {
public:
    JobQueue() = default;

    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    // Appends one job to the back. Throws std::logic_error if the queue has
    // already been closed.
    void push(QueuedJob job);

    // Blocks until a job is available or the queue is closed. Returns the
    // oldest job, or std::nullopt once the queue is closed and empty.
    std::optional<QueuedJob> pop();

    // Like pop(), but returns std::nullopt after `timeout` when no job has
    // arrived yet. Callers can use the timeout to check worker liveness. A
    // closed queue still returns queued jobs; check closed() to distinguish an
    // empty closed queue from an ordinary timeout.
    std::optional<QueuedJob> pop_for(std::chrono::milliseconds timeout);

    // Puts a job back at the front of the queue. This is used when a job was
    // claimed but could not be delivered, so it has not started running and
    // must be offered to another worker. Unlike push(), this is allowed after
    // close() because it returns existing work rather than adding new work.
    void requeue_front(QueuedJob job);

    // Stops accepting new jobs and wakes every blocked pop(). Jobs already
    // queued remain available until they are popped.
    void close();

    bool empty() const;
    std::size_t size() const;
    bool closed() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueuedJob> jobs_;
    bool closed_ = false;
};
