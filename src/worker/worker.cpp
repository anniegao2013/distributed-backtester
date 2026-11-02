#include "worker/worker.h"

#include <cstddef>
#include <exception>
#include <string>
#include <utility>

namespace {

void log(const WorkerConfig& config, const std::string& text) {
    if (config.log) {
        config.log(text);
    }
}

void send_worker_error(
    WorkerTransport& transport,
    const WorkerConfig& config,
    const std::string& type,
    const std::string& message,
    const std::string& path,
    const std::string& message_id
) {
    transport.send(
        worker_error_to_json(
            make_worker_error(
                type,
                message,
                path,
                message_id,
                config.worker_id
            ),
            /*pretty=*/false
        )
    );
}

std::string ready_message_id(const std::string& worker_id) {
    // message_id and worker_id share the protocol's 128-character limit. Keep
    // the announcement correlatable without allowing a maximum-length worker
    // id to push the suffix over the limit.
    constexpr std::size_t kMaxIdentifierLength = 128;
    const std::string suffix = ":ready:0001";

    if (worker_id.size() + suffix.size() <= kMaxIdentifierLength) {
        return worker_id + suffix;
    }

    return worker_id.substr(
        0,
        kMaxIdentifierLength - suffix.size()
    ) + suffix;
}

std::string count_job(WorkerStats& stats, const WorkerJobOutcome& outcome) {
    if (outcome.status == "completed") {
        ++stats.jobs_completed;
    } else if (outcome.status == "invalid") {
        ++stats.jobs_invalid;
    } else {
        ++stats.jobs_failed;
    }

    return outcome.status;
}

}  // namespace

WorkerStats run_worker(
    WorkerTransport& transport,
    const WorkerConfig& config
) {
    WorkerStats stats;

    transport.connect();

    WorkerReadyMessage ready;
    ready.message_id = ready_message_id(config.worker_id);
    ready.worker_id = config.worker_id;
    transport.send(worker_ready_to_json(ready, /*pretty=*/false));
    ++stats.ready_messages_sent;

    log(
        config,
        "worker '" + config.worker_id +
            "' connected; sent worker_ready; waiting for jobs"
    );

    while (const auto text = transport.receive()) {
        ++stats.messages_received;

        // Read the envelope first. It is cheap, and it lets the worker echo
        // message_id even when the version or payload cannot be handled.
        ProtocolEnvelope envelope;

        try {
            envelope = parse_protocol_envelope(*text);
        } catch (const JsonParseError& error) {
            send_worker_error(
                transport,
                config,
                "json_parse_error",
                error.what(),
                "",
                ""
            );
            ++stats.worker_errors;
            log(config, "rejected message: malformed JSON");
            continue;
        } catch (const ProtocolValidationError& error) {
            send_worker_error(
                transport,
                config,
                "validation_error",
                error.what(),
                error.path(),
                ""
            );
            ++stats.worker_errors;
            log(config, "rejected message: invalid envelope at " +
                error.path());
            continue;
        }

        if (envelope.protocol_version != kBacktestProtocolVersion) {
            send_worker_error(
                transport,
                config,
                "unsupported_version",
                "Unsupported protocol_version " +
                    std::to_string(envelope.protocol_version) +
                    "; this build speaks version " +
                    std::to_string(kBacktestProtocolVersion),
                "$.protocol_version",
                envelope.message_id
            );
            ++stats.worker_errors;
            log(config, "rejected message: unsupported protocol version");
            continue;
        }

        if (envelope.type == "shutdown") {
            try {
                const ShutdownMessage shutdown =
                    parse_shutdown_json(*text);

                ++stats.shutdowns_received;
                log(
                    config,
                    "shutdown requested" +
                        (shutdown.reason.empty()
                             ? std::string()
                             : ": " + shutdown.reason)
                );
                break;
            } catch (const JsonParseError& error) {
                send_worker_error(
                    transport,
                    config,
                    "json_parse_error",
                    error.what(),
                    "",
                    envelope.message_id
                );
                ++stats.worker_errors;
                log(config, "rejected shutdown: malformed JSON");
                continue;
            } catch (const ProtocolValidationError& error) {
                send_worker_error(
                    transport,
                    config,
                    "validation_error",
                    error.what(),
                    error.path(),
                    envelope.message_id
                );
                ++stats.worker_errors;
                log(
                    config,
                    "rejected shutdown: invalid payload at " +
                        error.path()
                );
                continue;
            }
        }

        if (envelope.type != "backtest_job") {
            send_worker_error(
                transport,
                config,
                "unknown_message_type",
                "Expected 'backtest_job' or 'shutdown', got '" +
                    envelope.type + "'",
                "$.type",
                envelope.message_id
            );
            ++stats.worker_errors;
            log(
                config,
                "rejected message: unexpected type '" + envelope.type + "'"
            );
            continue;
        }

        try {
            const BacktestJobMessage job = parse_backtest_job_json(*text);
            ++stats.jobs_received;

            const WorkerJobOutcome outcome =
                execute_backtest_job(job.job);

            transport.send(
                backtest_result_to_json(
                    make_result_message(
                        job,
                        config.worker_id,
                        outcome
                    ),
                    /*pretty=*/false
                )
            );

            log(
                config,
                "job " + job.job.job_id + " " +
                    count_job(stats, outcome) + " in " +
                    std::to_string(outcome.duration_seconds) + "s"
            );
        } catch (const JsonParseError& error) {
            send_worker_error(
                transport,
                config,
                "json_parse_error",
                error.what(),
                "",
                envelope.message_id
            );
            ++stats.worker_errors;
            log(config, "rejected job: malformed JSON");
        } catch (const ProtocolValidationError& error) {
            // The envelope was readable but the job payload was not. The
            // coordinator can correlate this reply through message_id.
            send_worker_error(
                transport,
                config,
                "validation_error",
                error.what(),
                error.path(),
                envelope.message_id
            );
            ++stats.worker_errors;
            log(
                config,
                "rejected job: invalid payload at " + error.path()
            );
        }
    }

    log(
        config,
        "worker '" + config.worker_id + "' stopped after " +
            std::to_string(stats.messages_received) + " message(s)"
    );

    return stats;
}
