#pragma once

#include "coordinator/scheduling.h"
#include "protocol/protocol.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

// The unit stored in the coordinator's job queue. The index preserves the
// position of the job in the finalized experiment's request order; outcomes are
// written back to that slot. `attempts` is the number of dispatch attempts
// already made: 0 before the first delivery, 1 after it, and so on. A retry
// keeps the same index and count until its delivery succeeds, and carries a new
// message_id for the upcoming attempt.
struct QueuedJob {
    std::size_t index = 0;
    BacktestJobMessage message;
    std::size_t attempts = 0;
};

// Thread-safe pending-job queue used to distribute jobs across workers.
//
// The coordinator pushes every expanded job once, then closes the queue.
// Workers call pop() to take the next job according to the configured
// SchedulingPolicy. FIFO is the default and preserves request order; LIFO and
// the two estimated-duration policies reorder pending work. A blocked pop()
// wakes when a job arrives or when close() is called.
class JobQueue {
public:
    explicit JobQueue(
        SchedulingPolicy policy = SchedulingPolicy::Fifo
    ) : policy_(policy) {}

    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    // Appends one job. Throws std::logic_error if the queue has already been
    // closed. This is the initial insertion path; initial work is appended in
    // request order and policy chooses which end or estimate to pop next.
    void push(QueuedJob job);

    // Blocks until a job is available or the queue is closed. Returns the next
    // job under the configured policy, or std::nullopt once the queue is
    // closed and empty.
    std::optional<QueuedJob> pop();

    // Like pop(), but returns std::nullopt after `timeout` when no job has
    // arrived yet. Callers can use the timeout to check worker liveness. A
    // closed queue still returns queued jobs; check closed() to distinguish an
    // empty closed queue from an ordinary timeout.
    std::optional<QueuedJob> pop_for(std::chrono::milliseconds timeout);

    // Puts a job back at the logical front of the queue. This is used when a
    // job was claimed but could not be delivered, so it has not started
    // running and must be offered to another worker again. Under FIFO and LIFO
    // it becomes the next job; under the priority policies the estimate still
    // decides, with insertion position breaking ties. Unlike push(), this is
    // allowed after close() because it returns existing work rather than
    // adding new work.
    void requeue_front(QueuedJob job);

    // Appends a failed attempt to the logical back of the queue. Retries go to
    // the back so other queued work keeps making progress and, when several
    // workers are available, the retry can land on a different worker. Under
    // FIFO and the priority policies that means the deque back; under LIFO it
    // means the deque front, which is popped last. Like requeue_front(), this
    // is allowed after close().
    void requeue_back(QueuedJob job);

    // Stops accepting new jobs and wakes every blocked pop(). Jobs already
    // queued remain available until they are popped.
    void close();

    bool empty() const;
    std::size_t size() const;
    bool closed() const;

    SchedulingPolicy policy() const noexcept { return policy_; }

private:
    // Returns the index of the next job to pop. The caller must hold mutex_.
    std::size_t next_index() const;

    // Removes and returns the next job. The caller must hold mutex_.
    std::optional<QueuedJob> take_next();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueuedJob> jobs_;
    SchedulingPolicy policy_ = SchedulingPolicy::Fifo;
    bool closed_ = false;
};
