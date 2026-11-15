#include "worker/dataset_cache.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

const std::string HEADER =
    "Date,Open,High,Low,Close,Adj.Close,Volume\n";

const std::string ONE_BAR =
    HEADER + "2018-01-02,10,12,9,11,11,1000\n";

const std::string TWO_BARS =
    ONE_BAR + "2018-01-03,11,13,10,12,12,1500\n";

const fs::path TEST_DIR =
    fs::temp_directory_path() / "distributed_backtester_dataset_cache_tests";

int passed = 0;
int failed = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream file(path);

    if (!file) {
        throw std::runtime_error(
            "Could not create test dataset " + path.string()
        );
    }

    file << contents;
}

bool load_throws(DatasetCache& cache, const std::string& path) {
    try {
        (void)cache.load(path);
    } catch (const std::exception&) {
        return true;
    }

    return false;
}

void run_test(
    const std::string& name,
    const std::function<void()>& test
) {
    try {
        test();
        ++passed;
        std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
        ++failed;
        std::cout << "[FAIL] " << name << ": " << error.what() << '\n';
    }
}

void test_first_load_misses_and_second_hits() {
    const fs::path file = TEST_DIR / "reuse.csv";
    write_file(file, ONE_BAR);

    DatasetCache cache;

    expect(
        cache.max_entries() == DatasetCache::kDefaultMaxEntries,
        "default capacity must match the documented default"
    );

    const std::vector<MarketBar>& first = cache.load(file.string());

    expect(first.size() == 1, "first load must return one bar");
    expect(cache.size() == 1, "first load must populate the cache");
    expect(cache.misses() == 1, "first load must be a miss");
    expect(cache.hits() == 0, "first load must not be a hit");

    const std::vector<MarketBar>& second = cache.load(file.string());

    expect(
        &first == &second,
        "a hit must return the retained dataset, not a copy"
    );
    expect(cache.size() == 1, "a hit must not add an entry");
    expect(cache.misses() == 1, "a hit must not count as a miss");
    expect(cache.hits() == 1, "second load must be a hit");
}

void test_normalized_paths_share_an_entry() {
    const fs::path file = TEST_DIR / "alias.csv";
    write_file(file, ONE_BAR);

    const fs::path relative = fs::relative(file, fs::current_path());

    DatasetCache cache;
    cache.load(file.string());
    cache.load(relative.string());

    expect(
        cache.size() == 1,
        "the same file under two path spellings must share an entry"
    );
    expect(cache.misses() == 1, "the alias must not be a second miss");
    expect(cache.hits() == 1, "the alias must resolve as a hit");
}

void test_distinct_files_are_distinct_entries() {
    const fs::path first = TEST_DIR / "distinct-a.csv";
    const fs::path second = TEST_DIR / "distinct-b.csv";
    write_file(first, ONE_BAR);
    write_file(second, TWO_BARS);

    DatasetCache cache;
    cache.load(first.string());
    const std::vector<MarketBar>& bars = cache.load(second.string());

    expect(bars.size() == 2, "the second dataset must be loaded");
    expect(cache.size() == 2, "both datasets must be retained");
    expect(cache.misses() == 2, "each dataset is its own miss");
    expect(cache.hits() == 0, "neither dataset was reused");
}

void test_hit_refreshes_recency() {
    const fs::path a = TEST_DIR / "lru-a.csv";
    const fs::path b = TEST_DIR / "lru-b.csv";
    const fs::path c = TEST_DIR / "lru-c.csv";
    write_file(a, ONE_BAR);
    write_file(b, ONE_BAR);
    write_file(c, ONE_BAR);

    DatasetCache cache(2);

    cache.load(a.string());
    cache.load(b.string());
    cache.load(a.string());  // Hit; a becomes the most recently used.
    cache.load(c.string());  // Miss; b is now the least recently used.

    expect(cache.size() == 2, "cache must stay at capacity");
    expect(cache.hits() == 1, "reloading a must be a hit");
    expect(cache.misses() == 3, "a, b, and c are three misses");

    cache.load(a.string());

    expect(cache.hits() == 2, "a must survive eviction as the MRU entry");

    cache.load(b.string());

    expect(
        cache.misses() == 4,
        "b must have been evicted as the least recently used entry"
    );
}

void test_capacity_is_enforced() {
    const fs::path first = TEST_DIR / "capacity-a.csv";
    const fs::path second = TEST_DIR / "capacity-b.csv";
    write_file(first, ONE_BAR);
    write_file(second, ONE_BAR);

    DatasetCache cache(1);
    cache.load(first.string());
    cache.load(second.string());

    expect(cache.size() == 1, "cache must not exceed max_entries");
    expect(cache.misses() == 2, "both datasets were loaded");
    expect(cache.max_entries() == 1, "capacity accessor must match");
}

void test_clear_drops_entries_but_keeps_counters() {
    const fs::path file = TEST_DIR / "clear.csv";
    write_file(file, ONE_BAR);

    DatasetCache cache;
    cache.load(file.string());
    cache.clear();

    expect(cache.size() == 0, "clear must drop every entry");

    cache.load(file.string());

    expect(cache.size() == 1, "the dataset must load again after clear");
    expect(cache.misses() == 2, "counters are cumulative across clear");
    expect(cache.hits() == 0, "a cleared dataset is not a hit");
}

void test_failed_load_is_not_cached() {
    DatasetCache cache;

    expect(
        load_throws(cache, (TEST_DIR / "missing.csv").string()),
        "a missing dataset must throw"
    );
    expect(cache.size() == 0, "a failed load must not be cached");
    expect(cache.misses() == 1, "a failed load counts as a miss");
    expect(cache.hits() == 0, "a failed load is not a hit");
}

void test_load_succeeds_after_a_failure() {
    const fs::path file = TEST_DIR / "recovered.csv";
    write_file(file, HEADER);

    DatasetCache cache;

    expect(
        load_throws(cache, file.string()),
        "a header-only dataset must throw"
    );
    expect(cache.size() == 0, "a rejected dataset must not be cached");

    write_file(file, ONE_BAR);

    const std::vector<MarketBar>& bars = cache.load(file.string());

    expect(bars.size() == 1, "the repaired dataset must load");
    expect(cache.size() == 1, "the repaired dataset must be cached");
    expect(cache.misses() == 2, "both attempts are misses");
    expect(cache.hits() == 0, "neither attempt was a hit");
}

void test_rewritten_file_is_reloaded() {
    const fs::path file = TEST_DIR / "rewritten.csv";
    write_file(file, ONE_BAR);

    DatasetCache cache;

    expect(
        cache.load(file.string()).size() == 1,
        "the original dataset must load"
    );

    write_file(file, TWO_BARS);

    const std::vector<MarketBar>& bars = cache.load(file.string());

    expect(bars.size() == 2, "a rewritten dataset must be reloaded");
    expect(cache.misses() == 2, "the rewrite must invalidate the entry");
    expect(cache.hits() == 0, "the stale entry must not be reused");
}

void test_zero_capacity_is_rejected() {
    bool threw = false;

    try {
        DatasetCache cache(0);
        (void)cache;
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    expect(threw, "max_entries must be at least 1");
}

}  // namespace

int main() {
    fs::remove_all(TEST_DIR);
    fs::create_directories(TEST_DIR);

    run_test(
        "first load misses and second hits",
        test_first_load_misses_and_second_hits
    );
    run_test(
        "normalized paths share an entry",
        test_normalized_paths_share_an_entry
    );
    run_test(
        "distinct files are distinct entries",
        test_distinct_files_are_distinct_entries
    );
    run_test("hit refreshes recency", test_hit_refreshes_recency);
    run_test("capacity is enforced", test_capacity_is_enforced);
    run_test(
        "clear drops entries but keeps counters",
        test_clear_drops_entries_but_keeps_counters
    );
    run_test(
        "failed load is not cached",
        test_failed_load_is_not_cached
    );
    run_test(
        "load succeeds after a failure",
        test_load_succeeds_after_a_failure
    );
    run_test(
        "rewritten file is reloaded",
        test_rewritten_file_is_reloaded
    );
    run_test("zero capacity is rejected", test_zero_capacity_is_rejected);

    fs::remove_all(TEST_DIR);

    std::cout << "\nResults: " << passed << " passed, "
              << failed << " failed\n";

    return failed == 0 ? 0 : 1;
}
