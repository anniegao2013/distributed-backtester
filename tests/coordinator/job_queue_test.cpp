#include "coordinator/job_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Joins a thread during stack unwinding so a failed assertion reports the
// failure instead of terminating the process on a joinable std::thread.
class ThreadJoiner {
public:
    explicit ThreadJoiner(std::thread& thread) : thread_(thread) {}

    ~ThreadJoiner() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    ThreadJoiner(const ThreadJoiner&) = delete;
    ThreadJoiner& operator=(const ThreadJoiner&) = delete;

private:
    std::thread& thread_;
};

QueuedJob make_job(std::size_t index) {
    QueuedJob job;
    job.index = index;
    job.message.message_id = "message-" + std::to_string(index);
    job.message.job.job_id = "job-" + std::to_string(index);
    return job;
}

void test_fifo_order() {
    JobQueue queue;

    for (std::size_t i = 0; i < 3; ++i) {
        queue.push(make_job(i));
    }

    expect(!queue.empty(), "queue must hold the pushed jobs");
    expect(queue.size() == 3, "queue size must match the push count");

    queue.close();

    expect(queue.closed(), "close must mark the queue closed");
    expect(!queue.empty(), "close must not discard queued jobs");

    for (std::size_t i = 0; i < 3; ++i) {
        const std::optional<QueuedJob> job = queue.pop();

        expect(job.has_value(), "a closed queue must still yield queued jobs");
        expect(
            job->index == i,
            "jobs must be popped in FIFO insertion order"
        );
    }

    expect(
        !queue.pop().has_value(),
        "pop must return nullopt once a closed queue is empty"
    );
    expect(queue.empty(), "draining the queue must leave it empty");
    expect(queue.size() == 0, "draining the queue must reset its size");
}

void test_pop_blocks_until_push() {
    JobQueue queue;

    std::promise<void> started;
    std::future<void> ready = started.get_future();
    std::promise<void> completed_promise;
    std::future<void> completed = completed_promise.get_future();
    std::optional<QueuedJob> result;

    std::thread consumer([&] {
        started.set_value();
        result = queue.pop();
        completed_promise.set_value();
    });
    ThreadJoiner consumer_joiner(consumer);

    ready.wait();

    expect(
        completed.wait_for(std::chrono::milliseconds(50)) ==
            std::future_status::timeout,
        "pop must block while the queue is open and empty"
    );

    queue.push(make_job(7));
    consumer.join();

    expect(
        completed.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready,
        "push must wake a blocked pop"
    );
    expect(result.has_value(), "the woken pop must return the pushed job");
    expect(result->index == 7, "the woken pop must return the new job");

    queue.close();
}

void test_close_wakes_blocked_pop() {
    JobQueue queue;

    std::promise<void> started;
    std::future<void> ready = started.get_future();
    std::promise<void> completed_promise;
    std::future<void> completed = completed_promise.get_future();
    std::optional<QueuedJob> result;

    std::thread consumer([&] {
        started.set_value();
        result = queue.pop();
        completed_promise.set_value();
    });
    ThreadJoiner consumer_joiner(consumer);

    ready.wait();

    expect(
        completed.wait_for(std::chrono::milliseconds(50)) ==
            std::future_status::timeout,
        "pop must block while the queue is open and empty"
    );

    queue.close();
    consumer.join();

    expect(
        completed.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready,
        "close must wake a blocked pop"
    );
    expect(
        !result.has_value(),
        "a woken pop must report exhaustion when no jobs remain"
    );
}

void test_push_after_close_is_rejected() {
    JobQueue queue;
    queue.close();

    bool threw = false;

    try {
        queue.push(make_job(0));
    } catch (const std::logic_error&) {
        threw = true;
    }

    expect(threw, "push after close must throw std::logic_error");
    expect(queue.empty(), "a rejected push must not add a job");
}

void test_pop_for_times_out() {
    JobQueue queue;

    const auto start = std::chrono::steady_clock::now();
    const std::optional<QueuedJob> job =
        queue.pop_for(std::chrono::milliseconds(40));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start
    );

    expect(!job.has_value(), "pop_for must return nullopt on timeout");
    expect(!queue.closed(), "a timeout must not close the queue");
    expect(
        elapsed.count() >= 30,
        "pop_for must wait for approximately its timeout"
    );
}

void test_pop_for_wakes_for_a_job() {
    JobQueue queue;

    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        queue.push(make_job(9));
    });

    const std::optional<QueuedJob> job =
        queue.pop_for(std::chrono::seconds(2));

    producer.join();

    expect(job.has_value(), "pop_for must wake when a job arrives");
    expect(job->index == 9, "pop_for must return the arriving job");

    queue.close();
}

void test_requeue_front_returns_work() {
    JobQueue queue;

    queue.push(make_job(0));
    queue.push(make_job(1));
    queue.close();

    const std::optional<QueuedJob> first = queue.pop();
    expect(first.has_value(), "the queue must yield its first job");
    expect(first->index == 0, "the first pop must be FIFO");

    // A claimed job that could not be delivered goes back to the front, even
    // after the queue has been closed for new work.
    queue.requeue_front(std::move(*first));

    const std::optional<QueuedJob> again = queue.pop();
    expect(again.has_value(), "the requeued job must be available");
    expect(again->index == 0, "the requeued job must stay at the front");

    const std::optional<QueuedJob> next = queue.pop();
    expect(next.has_value(), "the original second job must remain");
    expect(next->index == 1, "requeueing must not reorder other jobs");

    expect(
        !queue.pop().has_value(),
        "the queue must be empty after the requeued work is consumed"
    );
}

void test_requeue_back_appends_retry() {
    JobQueue queue;

    queue.push(make_job(0));
    queue.push(make_job(1));
    queue.push(make_job(2));
    queue.close();

    const std::optional<QueuedJob> first = queue.pop();
    expect(first.has_value(), "the queue must yield its first job");
    expect(first->index == 0, "the first pop must be FIFO");

    QueuedJob retry = std::move(*first);
    retry.attempts = 1;
    retry.message.message_id = "message-0-retry";

    // Retries go to the back so other queued work keeps progressing.
    queue.requeue_back(std::move(retry));

    for (std::size_t expected : {std::size_t{1}, std::size_t{2}}) {
        const std::optional<QueuedJob> next = queue.pop();
        expect(next.has_value(), "queued work must remain available");
        expect(
            next->index == expected,
            "a retry must not jump ahead of queued work"
        );
    }

    const std::optional<QueuedJob> retried = queue.pop();
    expect(retried.has_value(), "the retried job must remain available");
    expect(retried->index == 0, "the retried job must be the last item");
    expect(retried->attempts == 1, "the retry must keep its attempt count");
    expect(
        retried->message.message_id == "message-0-retry",
        "the retry must keep its new correlation key"
    );

    expect(
        !queue.pop().has_value(),
        "the queue must be empty after every retry is consumed"
    );
}

void test_concurrent_distribution() {
    constexpr std::size_t kJobCount = 200;
    constexpr std::size_t kConsumerCount = 8;

    JobQueue queue;

    for (std::size_t i = 0; i < kJobCount; ++i) {
        queue.push(make_job(i));
    }

    queue.close();

    std::mutex mutex;
    std::exception_ptr consumer_error;
    std::vector<std::vector<std::size_t>> consumed(kConsumerCount);

    std::vector<std::thread> consumers;
    consumers.reserve(kConsumerCount);

    for (std::size_t i = 0; i < kConsumerCount; ++i) {
        consumers.emplace_back([&, i] {
            try {
                while (const std::optional<QueuedJob> job = queue.pop()) {
                    consumed[i].push_back(job->index);
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                consumer_error = std::current_exception();
            }
        });
    }

    for (std::thread& consumer : consumers) {
        consumer.join();
    }

    if (consumer_error) {
        std::rethrow_exception(consumer_error);
    }

    std::vector<std::size_t> all;
    all.reserve(kJobCount);

    for (const std::vector<std::size_t>& indices : consumed) {
        expect(
            std::is_sorted(indices.begin(), indices.end()),
            "each consumer must observe jobs in FIFO order"
        );

        all.insert(all.end(), indices.begin(), indices.end());
    }

    expect(
        all.size() == kJobCount,
        "concurrent consumers must receive every job exactly once"
    );

    std::sort(all.begin(), all.end());

    for (std::size_t i = 0; i < kJobCount; ++i) {
        expect(
            all[i] == i,
            "concurrent consumers must not duplicate or drop jobs"
        );
    }
}

}  // namespace

int main() {
    try {
        test_fifo_order();
        test_pop_blocks_until_push();
        test_close_wakes_blocked_pop();
        test_pop_for_times_out();
        test_pop_for_wakes_for_a_job();
        test_requeue_front_returns_work();
        test_requeue_back_appends_retry();
        test_push_after_close_is_rejected();
        test_concurrent_distribution();

        std::cout << "job_queue_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "job_queue_tests failed: " << error.what() << '\n';
        return 1;
    }
}
