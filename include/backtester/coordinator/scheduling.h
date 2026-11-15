#pragma once

#include "protocol/protocol.h"

#include <string>

// Order in which the coordinator offers pending jobs to workers.
//
// The policies only change local dispatch order; the wire protocol and worker
// behavior are unchanged. Results are still written back by request index, so
// the aggregated experiment document is identical for every policy.
enum class SchedulingPolicy {
    // First in, first out. This is the original coordinator behavior and the
    // only policy that needs no duration estimate.
    Fifo,

    // Last in, first out. Useful as a control baseline and for demonstrating
    // how stack-like dispatch changes makespan.
    Lifo,

    // Shortest estimated job first. Minimizes mean waiting time, but can leave
    // one long job running alone at the end.
    ShortestJobFirst,

    // Longest estimated job first (longest processing time). Approximates the
    // makespan-optimal greedy schedule when several workers are available.
    LongestJobFirst
};

// Canonical CLI/JSON token for a policy: "fifo", "lifo", "shortest-first", or
// "longest-first".
const char* scheduling_policy_name(SchedulingPolicy policy);

// Parses a policy token. Accepts the canonical names plus the aliases
// "sjf", "ljf", "shortest-job-first", and "longest-job-first". Returns false
// and leaves `policy` unchanged when the token is unknown.
bool parse_scheduling_policy(
    const std::string& text,
    SchedulingPolicy& policy
);

// Relative cost estimate for one job, derived only from its strategy type and
// parameters. It is a scheduling heuristic, not a predicted wall-clock time: a
// larger number means the job is expected to take longer.
//
// This estimate grew out of the original O(window)-per-bar indicators. Moving
// average and Donchian now update their channels and averages incrementally, so
// their measured cost is dominated by fixed per-bar engine work and lookback
// length no longer predicts it. The window-based estimate is retained as a
// coarse ordering heuristic; only its relative order matters to the queue.
double estimate_job_cost(const BacktestJob& job);
