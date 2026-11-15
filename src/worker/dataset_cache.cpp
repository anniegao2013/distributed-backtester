#include "worker/dataset_cache.h"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

// Identifies a dataset by normalized path and file identity. Size and
// modification time make a rewritten CSV a miss; a path that cannot be
// inspected falls back to the path alone, and load_market_data() then reports
// the real read error.
std::string cache_key(const std::string& filepath) {
    std::error_code error;
    std::filesystem::path canonical =
        std::filesystem::weakly_canonical(filepath, error);

    if (error) {
        canonical = filepath;
    }

    std::string key = canonical.string();

    error.clear();
    const auto size = std::filesystem::file_size(canonical, error);

    if (!error) {
        key += '|';
        key += std::to_string(size);
    }

    error.clear();
    const auto modified =
        std::filesystem::last_write_time(canonical, error);

    if (!error) {
        key += '|';
        key += std::to_string(
            static_cast<long long>(modified.time_since_epoch().count())
        );
    }

    return key;
}

}  // namespace

DatasetCache::DatasetCache(std::size_t max_entries)
    : max_entries_(max_entries) {
    if (max_entries_ == 0) {
        throw std::invalid_argument(
            "DatasetCache max_entries must be at least 1"
        );
    }
}

const std::vector<MarketBar>& DatasetCache::load(
    const std::string& filepath
) {
    const std::string key = cache_key(filepath);

    const auto match = std::find_if(
        entries_.begin(),
        entries_.end(),
        [&key](const Entry& entry) { return entry.key == key; }
    );

    if (match != entries_.end()) {
        ++hits_;
        // Move the hit to the front so the back is always the eviction
        // candidate.
        std::rotate(entries_.begin(), match, std::next(match));
        return entries_.front().bars;
    }

    ++misses_;

    // If parsing throws, entries_ is untouched, so a failure is not cached.
    std::vector<MarketBar> bars = load_market_data(filepath);

    entries_.insert(
        entries_.begin(),
        Entry{key, std::move(bars)}
    );

    if (entries_.size() > max_entries_) {
        entries_.pop_back();
    }

    return entries_.front().bars;
}

std::size_t DatasetCache::size() const noexcept {
    return entries_.size();
}

std::size_t DatasetCache::max_entries() const noexcept {
    return max_entries_;
}

std::size_t DatasetCache::hits() const noexcept {
    return hits_;
}

std::size_t DatasetCache::misses() const noexcept {
    return misses_;
}

void DatasetCache::clear() noexcept {
    entries_.clear();
}
