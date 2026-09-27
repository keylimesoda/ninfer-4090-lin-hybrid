#pragma once

// Optional full-precision JSONL measurement log. Human operational rendering is owned separately
// by operational_log.* and never consumes these serialized records.

#include "serve/request_events.h"
#include "serve/serve_options.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace spdlog {
class logger;
}

namespace ninfer::serve {

inline constexpr int kRequestLogSchemaVersion        = 20;
inline constexpr const char* kRequestLogArtifactType = "ninfer_serve_request_log";

struct ServerLogEnvironment {
    int device = 0;
    std::string gpu_name;
    std::string gpu_uuid;
    std::uint64_t total_device_memory_bytes = 0;
    int compute_capability_major            = 0;
    int compute_capability_minor            = 0;
    std::string cuda_compile_version;
    std::string cuda_runtime_version;
    std::string cuda_driver_version;
};

// Pure JSON formatters are public to repository tests. Each return value is one complete JSON
// object without a trailing newline.
std::string format_server_start_json(
    const std::string& server_instance_id, std::uint64_t timestamp_unix_ms,
    const ServeOptions& options, const ninfer::EngineOptions& engine_options,
    const ninfer::ModelSamplingDefaults& sampling_defaults, const std::string& public_model_id,
    const ninfer::LoadSummary& load, const ninfer::MemorySummary& memory,
    const ServerLogEnvironment& environment, std::optional<std::uint64_t> artifact_size_bytes);
std::string format_request_start_json(const std::string& server_instance_id,
                                      std::uint64_t timestamp_unix_ms,
                                      const RequestLogContext& context);
std::string format_request_rejected_json(const std::string& server_instance_id,
                                         std::uint64_t timestamp_unix_ms,
                                         const RequestRejectionLogContext& context);
std::string format_request_done_json(const std::string& server_instance_id,
                                     std::uint64_t timestamp_unix_ms,
                                     const RequestLogContext& context,
                                     const GenerationOutcome& outcome);
std::string format_request_error_json(const std::string& server_instance_id,
                                      std::uint64_t timestamp_unix_ms,
                                      const RequestLogContext& context, const std::string& message);
std::string format_throughput_json(const std::string& server_instance_id,
                                   std::uint64_t timestamp_unix_ms, const ThroughputReport& report);

ServerLogEnvironment query_server_log_environment(int device);

// Record timestamps and the per-process identity every record carries. Exposed because EventStream
// owns the funnel that stamps a record once and fans it out to its sinks.
std::uint64_t unix_time_ms();
std::string new_server_instance_id();

// Append-only file sink for already-formatted records. Opens in append mode so one campaign file
// can contain multiple independently started MTP/model blocks. It formats and stamps nothing:
// EventStream owns the schema instance and hands this class complete lines.
class JsonlRequestLog {
public:
    explicit JsonlRequestLog(const std::string& path,
                             const std::string& protected_artifact_path = {},
                             std::shared_ptr<spdlog::logger> logger     = {});

    JsonlRequestLog(const JsonlRequestLog&)            = delete;
    JsonlRequestLog& operator=(const JsonlRequestLog&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return output_.is_open(); }

    void write_record(const std::string& record);

private:
    std::string path_;
    std::ofstream output_;
    std::mutex mutex_;
    std::shared_ptr<spdlog::logger> logger_;
    bool failed_ = false;
};

} // namespace ninfer::serve
