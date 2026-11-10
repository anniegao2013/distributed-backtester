#include "experiment/result_collector.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <iostream>
#include <mutex>
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

ExperimentJob make_job(std::size_t index) {
    ExperimentJob job;
    job.job_id = "job-" + std::to_string(index);
    job.strategy_id = "strategy";
    job.strategy_type = "moving_average_crossover";
    job.parameters = {
        {"short_window", 5.0},
        {"long_window", 20.0}
    };
    return job;
}

std::vector<ExperimentJob> make_jobs(std::size_t count) {
    std::vector<ExperimentJob> jobs;
    jobs.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        jobs.push_back(make_job(i));
    }

    return jobs;
}

ExperimentJobOutcome completed_outcome(
    const ExperimentJob& job,
    double duration_seconds = 0.1
) {
    ExperimentJobOutcome outcome;
    outcome.job = job;
    outcome.ok = true;
    outcome.duration_seconds = duration_seconds;
    outcome.result.job_id = job.job_id;
    return outcome;
}

ExperimentJobOutcome failed_outcome(const ExperimentJob& job) {
    ExperimentJobOutcome outcome;
    outcome.job = job;
    outcome.error = "explicit failure";
    return outcome;
}

void test_out_of_order_results_are_returned_in_request_order() {
    const std::vector<ExperimentJob> jobs = make_jobs(3);
    ExperimentResultCollector collector(jobs);

    expect(collector.size() == 3, "collector size must match the job count");
    expect(!collector.complete(), "a new collector must be incomplete");
    expect(
        collector.remaining() == 3,
        "every job must start pending"
    );

    collector.record(2, completed_outcome(jobs[2]));
    collector.record(0, failed_outcome(jobs[0]));
    collector.record(1, completed_outcome(jobs[1]));

    expect(collector.complete(), "all three jobs must be terminal");
    expect(
        collector.terminal_count() == 3,
        "terminal_count must include completed and failed jobs"
    );
    expect(collector.remaining() == 0, "no job may remain pending");

    const std::vector<ExperimentJobOutcome> outcomes = collector.take();

    expect(outcomes.size() == 3, "take must return every outcome");

    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        expect(
            outcomes[i].job.job_id == jobs[i].job_id,
            "take must preserve request order"
        );
    }

    expect(!outcomes[0].ok, "job 0 was recorded as failed");
    expect(outcomes[1].ok, "job 1 was recorded as completed");
    expect(outcomes[2].ok, "job 2 was recorded as completed");
}

void test_wait_blocks_until_every_job_is_terminal() {
    const std::vector<ExperimentJob> jobs = make_jobs(3);
    ExperimentResultCollector collector(jobs);

    collector.record(0, completed_outcome(jobs[0]));
    collector.record(2, completed_outcome(jobs[2]));

    std::promise<void> started;
    std::future<void> ready = started.get_future();
    std::atomic<bool> finished{false};

    std::thread waiter([&] {
        started.set_value();
        collector.wait();
        finished.store(true);
    });

    ready.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    expect(
        !finished.load(),
        "wait must block while any job is still pending"
    );

    collector.record(1, failed_outcome(jobs[1]));
    waiter.join();

    expect(
        finished.load(),
        "wait must return once the final job becomes terminal"
    );
}

void test_duplicate_records_are_idempotent_and_range_is_rejected() {
    const std::vector<ExperimentJob> jobs = make_jobs(2);
    ExperimentResultCollector collector(jobs);

    expect(
        collector.record(0, completed_outcome(jobs[0])),
        "the first result must be stored"
    );

    // A duplicate must not overwrite the first outcome, advance the terminal
    // count, or change what take() returns.
    expect(
        !collector.record(0, failed_outcome(jobs[0])),
        "a duplicate result must be ignored"
    );
    expect(
        collector.terminal_count() == 1,
        "a duplicate must not advance the terminal count"
    );

    bool range_threw = false;

    try {
        collector.record(2, completed_outcome(jobs[0]));
    } catch (const std::out_of_range&) {
        range_threw = true;
    }

    expect(range_threw, "an invalid job index must be rejected");

    expect(
        collector.record(1, failed_outcome(jobs[1])),
        "a different pending job must still be recorded"
    );

    const std::vector<ExperimentJobOutcome> outcomes = collector.take();

    expect(
        outcomes[0].ok,
        "the first terminal outcome must win over a duplicate"
    );
    expect(!outcomes[1].ok, "the second job must keep its own outcome");
}

void test_take_requires_a_terminal_state_for_every_job() {
    const std::vector<ExperimentJob> jobs = make_jobs(2);
    ExperimentResultCollector collector(jobs);

    collector.record(0, completed_outcome(jobs[0]));

    bool threw = false;

    try {
        (void)collector.take();
    } catch (const std::logic_error&) {
        threw = true;
    }

    expect(threw, "take must reject an incomplete experiment");

    collector.record(1, failed_outcome(jobs[1]));

    const std::vector<ExperimentJobOutcome> outcomes = collector.take();
    expect(outcomes.size() == 2, "take must succeed once all jobs terminate");

    bool second_take_threw = false;

    try {
        (void)collector.take();
    } catch (const std::logic_error&) {
        second_take_threw = true;
    }

    expect(second_take_threw, "a collector may only be consumed once");
}

void test_fail_remaining_terminates_only_pending_jobs() {
    const std::vector<ExperimentJob> jobs = make_jobs(3);
    ExperimentResultCollector collector(jobs);

    collector.record(1, completed_outcome(jobs[1]));

    const std::size_t marked = collector.fail_remaining(
        "no worker was available"
    );

    expect(marked == 2, "fail_remaining must mark every pending job");
    expect(collector.complete(), "the experiment must now be complete");

    const std::vector<ExperimentJobOutcome> outcomes = collector.take();

    expect(outcomes[0].job.job_id == jobs[0].job_id, "job 0 must be kept");
    expect(!outcomes[0].ok, "a pending job must fail");
    expect(
        outcomes[0].error == "no worker was available",
        "a pending job must carry the supplied reason"
    );
    expect(outcomes[1].ok, "a completed job must not be overwritten");
    expect(outcomes[2].job.job_id == jobs[2].job_id, "job 2 must be kept");
    expect(!outcomes[2].ok, "a pending job must fail");
}

void test_fail_remaining_can_explain_each_job() {
    const std::vector<ExperimentJob> jobs = make_jobs(3);
    ExperimentResultCollector collector(jobs);

    collector.record(1, completed_outcome(jobs[1]));

    const std::size_t marked = collector.fail_remaining(
        [](std::size_t index) {
            return "job " + std::to_string(index) + " was abandoned";
        }
    );

    expect(marked == 2, "the callback form must mark every pending job");

    const std::vector<ExperimentJobOutcome> outcomes = collector.take();

    expect(
        outcomes[0].error == "job 0 was abandoned",
        "the callback message must use the request index"
    );
    expect(outcomes[1].ok, "a completed job must not be overwritten");
    expect(
        outcomes[2].error == "job 2 was abandoned",
        "each pending job must receive its own message"
    );
}

void test_concurrent_recording_preserves_every_job() {
    constexpr std::size_t kJobCount = 100;
    constexpr std::size_t kThreadCount = 8;

    const std::vector<ExperimentJob> jobs = make_jobs(kJobCount);
    ExperimentResultCollector collector(jobs);

    std::exception_ptr record_error;
    std::mutex mutex;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (std::size_t thread_index = 0;
         thread_index < kThreadCount;
         ++thread_index) {
        threads.emplace_back([&, thread_index] {
            try {
                // Fill one residue class per thread, then record the low
                // indices last so completion order differs from request order.
                for (std::size_t i = thread_index + kThreadCount;
                     i < kJobCount;
                     i += kThreadCount) {
                    collector.record(
                        i,
                        completed_outcome(jobs[i])
                    );
                }

                collector.record(
                    thread_index,
                    completed_outcome(jobs[thread_index])
                );
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                record_error = std::current_exception();
            }
        });
    }

    for (std::thread& thread : threads) {
        thread.join();
    }

    if (record_error) {
        std::rethrow_exception(record_error);
    }

    collector.wait();

    const std::vector<ExperimentJobOutcome> outcomes = collector.take();

    expect(
        outcomes.size() == kJobCount,
        "concurrent recording must produce one outcome per job"
    );

    for (std::size_t i = 0; i < kJobCount; ++i) {
        expect(
            outcomes[i].job.job_id == jobs[i].job_id,
            "concurrent outcomes must stay in request order"
        );
        expect(outcomes[i].ok, "every concurrent job must complete");
    }
}

}  // namespace

int main() {
    try {
        test_out_of_order_results_are_returned_in_request_order();
        test_wait_blocks_until_every_job_is_terminal();
        test_duplicate_records_are_idempotent_and_range_is_rejected();
        test_take_requires_a_terminal_state_for_every_job();
        test_fail_remaining_terminates_only_pending_jobs();
        test_fail_remaining_can_explain_each_job();
        test_concurrent_recording_preserves_every_job();

        std::cout << "result_collector_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "result_collector_tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
