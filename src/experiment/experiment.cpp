#include "experiment_internal.h"

#include <exception>
#include <string>
#include <utility>

using experiment_detail::error_document;

// -------------------------------------------------------------------
// Public API facade
// -------------------------------------------------------------------

ExperimentValidationError::ExperimentValidationError(
    std::string path,
    std::string message
) : std::runtime_error(std::move(message)), path_(std::move(path)) {}

const std::string& ExperimentValidationError::path() const noexcept {
    return path_;
}

std::string run_experiment_json(
    const std::string& request_json,
    bool pretty
) {
    try {
        const ExperimentRequest request =
            parse_experiment_json(request_json);

        const ExperimentResult result = run_experiment(request);
        return experiment_result_to_json(result, pretty);
    } catch (const JsonParseError& error) {
        return serialize_json(
            error_document(
                "invalid",
                "json_parse_error",
                "",
                error.what()
            ),
            pretty
        );
    } catch (const ExperimentValidationError& error) {
        return serialize_json(
            error_document(
                "invalid",
                "validation_error",
                error.path(),
                error.what()
            ),
            pretty
        );
    } catch (const std::exception& error) {
        return serialize_json(
            error_document(
                "failed",
                "runtime_error",
                "",
                error.what()
            ),
            pretty
        );
    }
}
