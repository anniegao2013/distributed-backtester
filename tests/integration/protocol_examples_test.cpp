#include "protocol/protocol.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string read_file(const std::string& path) {
    std::ifstream stream(path);

    if (!stream) {
        throw std::runtime_error("Could not open example: " + path);
    }

    std::ostringstream contents;
    contents << stream.rdbuf();

    if (!stream.good() && !stream.eof()) {
        throw std::runtime_error("Could not read example: " + path);
    }

    return contents.str();
}

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        const BacktestJobMessage job = parse_backtest_job_json(
            read_file("examples/protocol/backtest_job.json")
        );
        const BacktestResultMessage result = parse_backtest_result_json(
            read_file("examples/protocol/backtest_result.json")
        );
        const BacktestResultMessage failed =
            parse_backtest_result_json(
                read_file(
                    "examples/protocol/backtest_result_failed.json"
                )
            );
        const BacktestResultMessage invalid =
            parse_backtest_result_json(
                read_file(
                    "examples/protocol/backtest_result_invalid.json"
                )
            );
        const ProtocolErrorMessage error = parse_protocol_error_json(
            read_file("examples/protocol/protocol_error.json")
        );

        expect(
            result.message_id == job.message_id,
            "result must correlate with the job sample"
        );
        expect(
            result.result.experiment_id == job.job.experiment_id &&
                result.result.job_id == job.job.job_id &&
                result.result.strategy_id == job.job.strategy_id &&
                result.result.strategy_type == job.job.strategy_type,
            "result identity must echo the job"
        );
        expect(
            result.result.parameters == job.job.parameters,
            "result parameters must echo the job"
        );
        expect(
            job.job.output.include_trades &&
                result.result.trades.has_value() &&
                !result.result.trades->empty(),
            "the sample job asks for trades and the sample result has them"
        );
        expect(
            error.message_id == job.message_id,
            "the protocol error sample correlates with the job sample"
        );
        expect(
            failed.result.status == "failed" &&
                failed.result.error.has_value() &&
                failed.result.error->type == "runtime_error" &&
                !failed.result.metrics.has_value(),
            "the failed sample carries a runtime error and no metrics"
        );
        expect(
            invalid.result.status == "invalid" &&
                invalid.result.error.has_value() &&
                invalid.result.error->type == "validation_error" &&
                !invalid.result.error->path.empty(),
            "the invalid sample carries a validation error with a path"
        );

        // Every example must survive a wire round-trip.
        (void)parse_backtest_job_json(backtest_job_to_json(job, false));
        (void)parse_backtest_result_json(
            backtest_result_to_json(result, false)
        );
        (void)parse_backtest_result_json(
            backtest_result_to_json(failed, false)
        );
        (void)parse_backtest_result_json(
            backtest_result_to_json(invalid, false)
        );
        (void)parse_protocol_error_json(
            protocol_error_to_json(error, false)
        );

        std::cout << "[PASS] protocol examples parse and round-trip\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
