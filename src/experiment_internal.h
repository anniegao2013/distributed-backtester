#pragma once

#include "experiment.h"
#include "json_value.h"

#include <cstddef>
#include <string>
#include <utility>

// Private declarations shared by the experiment implementation files. This
// header is intentionally kept under src/ so the public surface remains
// experiment.h.
namespace experiment_detail {

inline constexpr const char* kRootPath = "$";
inline constexpr const char* kSchemaVersion = "1";

// A single request may not spawn an unbounded number of worker threads.
inline constexpr double kMaxConcurrentJobs = 1024.0;

[[noreturn]] inline void fail(
    const std::string& path,
    const std::string& message
) {
    throw ExperimentValidationError(path, message);
}

inline std::string child_path(
    const std::string& parent,
    const std::string& key
) {
    return parent + "." + key;
}

inline std::string index_path(
    const std::string& parent,
    std::size_t index
) {
    return parent + "[" + std::to_string(index) + "]";
}

// Validates the parameters for one strategy type and throws
// ExperimentValidationError on an unknown type, unknown parameter, missing
// parameter, or invalid value. Defined in strategy_registry.cpp.
void validate_strategy_parameters(
    const std::string& type,
    const ParameterMap& parameters,
    const std::string& path
);

// Builds a schema-shaped error response. Defined in
// experiment_result_json.cpp.
JsonValue error_document(
    const std::string& status,
    const std::string& type,
    const std::string& path,
    const std::string& message
);

}  // namespace experiment_detail
