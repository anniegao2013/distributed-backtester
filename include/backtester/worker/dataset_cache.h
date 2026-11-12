#pragma once

#include "core/market_data.h"

#include <cstddef>
#include <string>
#include <vector>

// Per-worker cache of parsed market datasets.
//
// A worker may run many jobs against the same CSV. Parsing that file once per
// job dominates worker-side runtime, so the worker keeps the parsed bars and
// reuses them across jobs. Entries are keyed by the normalized path plus the
// file's size and modification time, so a rewritten dataset is reloaded instead
// of silently serving stale bars.
//
// The cache is a small most-recently-used list bounded by `max_entries`. It is
// intentionally not thread-safe: one worker executes at most one job at a time.
class DatasetCache {
public:
    static constexpr std::size_t kDefaultMaxEntries = 4;

    // `max_entries` bounds the number of datasets retained at once and must be
    // at least 1.
    explicit DatasetCache(
        std::size_t max_entries = kDefaultMaxEntries
    );

    // Returns the parsed bars for `filepath`, loading them on a miss. The
    // returned reference stays valid until the next load() or clear() on this
    // cache. Throws if the dataset cannot be read or parsed; a failed load is
    // not cached.
    const std::vector<MarketBar>& load(const std::string& filepath);

    // Number of datasets currently cached.
    std::size_t size() const noexcept;

    // Bound supplied at construction.
    std::size_t max_entries() const noexcept;

    // Cumulative counters over the lifetime of this cache, including loads
    // that later failed.
    std::size_t hits() const noexcept;
    std::size_t misses() const noexcept;

    // Drops every cached dataset. Counters are cumulative and are not reset.
    void clear() noexcept;

private:
    struct Entry {
        std::string key;
        std::vector<MarketBar> bars;
    };

    std::size_t max_entries_;
    std::vector<Entry> entries_;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
};
