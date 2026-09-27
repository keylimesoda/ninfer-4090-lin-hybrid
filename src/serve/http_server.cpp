#include "serve/http_server.h"

#include <spdlog/logger.h>

#include "serve/anthropic_messages.h"
#include "serve/http_transport.h"
#include "serve/openai_common.h"
#include "serve/request_log.h"
#include "serve/slot_files.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ninfer::serve {
namespace {

std::string format_seconds(double seconds) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", seconds);
    return text;
}
void write_exception(httplib::Response& res, const std::exception& ex) {
    ApiError error;
    error.status  = 500;
    error.type    = "internal_error";
    error.message = ex.what();
    write_openai_error(res, error);
}

bool is_anthropic_path(std::string_view path) { return path.starts_with("/v1/messages"); }

bool is_openai_path(std::string_view path) {
    return path.starts_with("/v1/") && !is_anthropic_path(path);
}

void ensure_openai_request_id(const httplib::Request& request, httplib::Response& response) {
    if (is_openai_path(request.path) && !response.has_header("x-request-id")) {
        response.set_header("x-request-id", new_openai_request_id());
    }
}

ThroughputReport make_throughput_report(const ninfer::RuntimeStats& previous,
                                        const ninfer::RuntimeStats& current,
                                        double interval_seconds) {
    return ThroughputReport{
        .interval_seconds = interval_seconds,
        .computed_prefill_tokens =
            current.computed_prefill_tokens - previous.computed_prefill_tokens,
        .committed_decode_tokens =
            current.committed_decode_tokens - previous.committed_decode_tokens,
        .decode_rounds     = current.decode_rounds - previous.decode_rounds,
        .decode_row_rounds = current.decode_row_rounds - previous.decode_row_rounds,
        .previous          = previous,
        .current           = current,
    };
}

bool report_has_activity(const ThroughputReport& report) {
    return report.computed_prefill_tokens != 0 || report.committed_decode_tokens != 0 ||
           report.decode_rounds != 0 || report.current.running_requests != 0 ||
           report.current.waiting_requests != 0 || report.current.materializing_requests != 0 ||
           report.current.capture_pending_requests != 0 ||
           report.current.terminal_pending_requests != 0 ||
           report.current.active_captures_completed != report.previous.active_captures_completed ||
           report.current.active_captures_aborted != report.previous.active_captures_aborted ||
           report.current.root_selections != report.previous.root_selections ||
           report.current.private_endpoint_selections !=
               report.previous.private_endpoint_selections ||
           report.current.private_turn_closure_selections !=
               report.previous.private_turn_closure_selections ||
           report.current.private_response_replay_selections !=
               report.previous.private_response_replay_selections ||
           report.current.private_long_anchor_selections !=
               report.previous.private_long_anchor_selections ||
           report.current.shared_stable_prefix_selections !=
               report.previous.shared_stable_prefix_selections ||
           report.current.state_moves != report.previous.state_moves ||
           report.current.state_forks != report.previous.state_forks ||
           report.current.state_restores != report.previous.state_restores ||
           report.current.state_d2h_count != report.previous.state_d2h_count ||
           report.current.state_h2d_count != report.previous.state_h2d_count ||
           report.current.state_d2d_count != report.previous.state_d2d_count ||
           report.current.main_kv_d2h_pages != report.previous.main_kv_d2h_pages ||
           report.current.main_kv_h2d_pages != report.previous.main_kv_h2d_pages ||
           report.current.main_kv_d2d_pages != report.previous.main_kv_d2d_pages ||
           report.current.backend_kv_d2h_pages != report.previous.backend_kv_d2h_pages ||
           report.current.backend_kv_h2d_pages != report.previous.backend_kv_h2d_pages ||
           report.current.backend_kv_d2d_pages != report.previous.backend_kv_d2d_pages ||
           report.current.pressure_spill_pages != report.previous.pressure_spill_pages ||
           report.current.partial_tail_cow_pages != report.previous.partial_tail_cow_pages ||
           report.current.pressure_private_owners_degraded !=
               report.previous.pressure_private_owners_degraded ||
           report.current.pressure_private_owners_evicted !=
               report.previous.pressure_private_owners_evicted ||
           report.current.pressure_shared_owners_degraded !=
               report.previous.pressure_shared_owners_degraded ||
           report.current.pressure_shared_owners_evicted !=
               report.previous.pressure_shared_owners_evicted ||
           report.current.pressure_checkpoints_dropped !=
               report.previous.pressure_checkpoints_dropped ||
           report.current.pressure_searches != report.previous.pressure_searches ||
           report.current.pressure_search_budget_exhaustions !=
               report.previous.pressure_search_budget_exhaustions ||
           report.current.pressure_maximal_fallback_selections !=
               report.previous.pressure_maximal_fallback_selections ||
           report.current.historical_fork_hits != report.previous.historical_fork_hits ||
           report.current.device_state_occupied_slots !=
               report.previous.device_state_occupied_slots ||
           report.current.host_state_occupied_slots != report.previous.host_state_occupied_slots ||
           report.current.device_main_kv_occupied_pages !=
               report.previous.device_main_kv_occupied_pages ||
           report.current.device_backend_kv_occupied_pages !=
               report.previous.device_backend_kv_occupied_pages ||
           report.current.host_kv_occupied_bytes != report.previous.host_kv_occupied_bytes ||
           report.current.shared_active_references != report.previous.shared_active_references ||
           report.current.host_work.engine_boundary_ns !=
               report.previous.host_work.engine_boundary_ns ||
           report.current.host_work.program_submit_ns !=
               report.previous.host_work.program_submit_ns ||
           report.current.host_work.program_post_ns != report.previous.host_work.program_post_ns ||
           report.current.host_work.engine_commit_output_ns !=
               report.previous.host_work.engine_commit_output_ns ||
           report.current.host_work.engine_maintenance_ns !=
               report.previous.host_work.engine_maintenance_ns ||
           report.current.host_work.device_wait_ns != report.previous.host_work.device_wait_ns;
}

const char* endpoint_name(std::string_view path) noexcept {
    if (path == "/v1/chat/completions") { return "openai_chat_completions"; }
    if (path == "/v1/responses") { return "openai_responses"; }
    if (path == "/v1/responses/input_tokens") { return "openai_responses_input_tokens"; }
    if (path == "/v1/messages") { return "anthropic_messages"; }
    if (path == "/v1/messages/count_tokens") { return "anthropic_count_tokens"; }
    return "http_route";
}

std::string response_request_id(const httplib::Response& response) {
    if (response.has_header("x-request-id")) { return response.get_header_value("x-request-id"); }
    if (response.has_header("request-id")) { return response.get_header_value("request-id"); }
    return {};
}

// Records retained for replay to a newly connected /events reader. At the default reporting
// interval this is roughly the last forty minutes of throughput samples interleaved with the
// request records from the same window - enough for a dashboard opened mid-run to draw a
// populated chart immediately rather than starting blank.
constexpr std::size_t kEventReplayCapacity = 512;

// SSE keepalive period. Also bounds how long a writer blocks before re-testing that its socket is
// still writable, which is how a closed browser tab is noticed on an idle server.
constexpr std::chrono::milliseconds kEventKeepalive{15000};

nlohmann::json arena_json(const ninfer::ArenaMemorySummary& arena) {
    return {{"capacity_bytes", arena.capacity_bytes},
            {"used_bytes", arena.used_bytes},
            {"peak_used_bytes", arena.peak_used_bytes}};
}

nlohmann::json gpu_json(const GpuTelemetry& gpu) {
    if (!gpu.available) { return {{"available", false}, {"error", gpu.error}}; }
    return {{"available", true},
            {"name", gpu.name},
            {"uuid", gpu.uuid},
            {"driver_version", gpu.driver_version},
            {"temperature_c", gpu.temperature_c},
            {"fan_percent", gpu.fan_percent},
            {"power_watts", gpu.power_watts},
            {"power_limit_watts", gpu.power_limit_watts},
            {"utilization_gpu_percent", gpu.utilization_gpu_percent},
            {"utilization_memory_percent", gpu.utilization_memory_percent},
            {"sm_clock_mhz", gpu.sm_clock_mhz},
            {"sm_clock_max_mhz", gpu.sm_clock_max_mhz},
            {"memory_clock_mhz", gpu.memory_clock_mhz},
            {"memory_used_bytes", gpu.memory_used_bytes},
            {"memory_total_bytes", gpu.memory_total_bytes},
            {"pcie_rx_bytes_per_second", gpu.pcie_rx_bytes_per_second},
            {"pcie_tx_bytes_per_second", gpu.pcie_tx_bytes_per_second},
            {"throttle_reasons", gpu.throttle_reasons}};
}

const char* kv_cache_name(ninfer::KvCacheStorage storage) noexcept {
    switch (storage) {
    case ninfer::KvCacheStorage::BFloat16: return "bf16";
    case ninfer::KvCacheStorage::Int8Group64: return "int8";
    case ninfer::KvCacheStorage::RotatedInt8KeyInt4ValueGroup64: return "rk8v4";
    case ninfer::KvCacheStorage::RotatedInt4KeyInt4ValueGroup64: return "rk4v4";
    case ninfer::KvCacheStorage::RK4V4E8: return "rk4v4-e8";
    case ninfer::KvCacheStorage::RK2V4E8: return "rk2v4-e8";
    case ninfer::KvCacheStorage::Fp8E4M3Row256: return "fp8-e4m3-256";
    case ninfer::KvCacheStorage::Nvfp4Group16: return "nvfp4-16";
    case ninfer::KvCacheStorage::Fp8KeyNvfp4Value: return "fp8-nvfp4";
    }
    return "unknown";
}

// One SSE frame carrying a complete request-log record. The record's own `event` field names
// the frame so a browser can attach one listener per record type.
std::string event_frame(std::string_view event, std::string_view payload) {
    std::string frame;
    frame.reserve(payload.size() + event.size() + 16);
    frame += "event: ";
    frame += event;
    frame += "\ndata: ";
    frame += payload;
    frame += "\n\n";
    return frame;
}

std::string_view record_event_name(const std::string& record) {
    // The formatters emit sorted keys, so `"event":"<name>"` is a stable substring. Parsing the
    // whole record again only to recover its type would double the cost of every frame.
    const std::size_t key = record.find("\"event\":\"");
    if (key == std::string::npos) { return "message"; }
    const std::size_t begin = key + 9;
    const std::size_t end   = record.find('"', begin);
    if (end == std::string::npos) { return "message"; }
    return std::string_view(record).substr(begin, end - begin);
}

} // namespace

void write_openai_error(httplib::Response& response, const ApiError& error) {
    response.status = error.status;
    response.set_content(make_error_body(error), "application/json");
}

void write_anthropic_error(httplib::Response& response, const ApiError& api_error,
                           const std::string& request_id) {
    const ApiError error = normalize_anthropic_error(api_error);
    response.status      = error.status;
    response.headers.erase("request-id");
    response.set_header("request-id", request_id);
    response.set_content(make_anthropic_error_body(error, request_id), "application/json");
}

httplib::Server::HandlerResponse handle_unrendered_http_error(const ServeOptions& options,
                                                              const httplib::Request& request,
                                                              httplib::Response& response) {
    ensure_openai_request_id(request, response);
    if (!response.body.empty()) { return httplib::Server::HandlerResponse::Unhandled; }

    ApiError error;
    if (response.status == 413) {
        error.status  = 413;
        error.type    = "invalid_request_error";
        error.code    = "request_too_large";
        error.message = "request body exceeds the configured payload limit of " +
                        std::to_string(options.max_request_bytes) + " bytes";
    } else if (response.status == 404 && request.path.rfind("/v1/messages", 0) == 0) {
        error.status  = 404;
        error.code    = "not_found";
        error.message = "requested Anthropic resource was not found";
    } else {
        return httplib::Server::HandlerResponse::Unhandled;
    }
    if (request.path.rfind("/v1/messages", 0) == 0) {
        write_anthropic_error(response, error, new_anthropic_request_id());
    } else {
        write_openai_error(response, error);
    }
    return httplib::Server::HandlerResponse::Handled;
}

bool matches_bearer_credential(std::string_view authorization, std::string_view api_key) noexcept {
    if (api_key.empty()) { return false; }
    const auto is_whitespace = [](char value) { return value == ' ' || value == '\t'; };
    const auto ascii_equal   = [](char lhs, char rhs) {
        if (lhs >= 'A' && lhs <= 'Z') { lhs = static_cast<char>(lhs - 'A' + 'a'); }
        if (rhs >= 'A' && rhs <= 'Z') { rhs = static_cast<char>(rhs - 'A' + 'a'); }
        return lhs == rhs;
    };

    std::size_t position = 0;
    while (position < authorization.size() && is_whitespace(authorization[position])) {
        ++position;
    }
    constexpr std::string_view scheme = "Bearer";
    if (authorization.size() - position < scheme.size()) { return false; }
    for (std::size_t index = 0; index < scheme.size(); ++index) {
        if (!ascii_equal(authorization[position + index], scheme[index])) { return false; }
    }
    position += scheme.size();
    if (position == authorization.size() || !is_whitespace(authorization[position])) {
        return false;
    }
    while (position < authorization.size() && is_whitespace(authorization[position])) {
        ++position;
    }
    std::size_t end = authorization.size();
    while (end > position && is_whitespace(authorization[end - 1])) { --end; }
    return authorization.substr(position, end - position) == api_key;
}

HttpServer::HttpServer(ServeOptions options, std::shared_ptr<spdlog::logger> logger)
    : options_(std::move(options)), openai_responses_store_(options_.response_store_max_records,
                                                            options_.response_store_max_bytes),
      logger_(logger), operational_log_(logger),
      events_(options_.request_log_jsonl, options_.artifact_path, kEventReplayCapacity,
              std::move(logger)),
      gpu_(options_.device) {
    const std::size_t queued_requests =
        static_cast<std::size_t>(options_.max_concurrency) + options_.max_pending_requests;
    const std::size_t worker_count = queued_requests + 1;
    server_.new_task_queue         = [queued_requests, worker_count] {
        return new httplib::ThreadPool(worker_count, worker_count, queued_requests);
    };
    server_.set_socket_options(configure_http_server_socket);
    server_.set_payload_max_length(options_.max_request_bytes);
    register_routes();
}

HttpServer::RequestLifecycle::RequestLifecycle(HttpServer& owner, RequestLogContext context)
    : owner_(&owner), context_(std::move(context)) {
    owner_->record_request_start(context_);
}

bool HttpServer::RequestLifecycle::claim(State terminal) noexcept {
    State expected = State::Pending;
    return state_.compare_exchange_strong(expected, terminal, std::memory_order_acq_rel);
}

void HttpServer::RequestLifecycle::done(const GenerationOutcome& outcome) {
    if (claim(State::Done)) { owner_->record_request_done(context_, outcome); }
}

void HttpServer::RequestLifecycle::failure(const RequestFailure& failure) {
    if (claim(State::Error)) { owner_->record_request_failure(context_, failure); }
}

void HttpServer::RequestLifecycle::response_failure(const RequestFailure& failure) {
    owner_->record_response_failure(context_.id, failure);
}

std::shared_ptr<HttpServer::RequestLifecycle> HttpServer::begin_request(RequestLogContext context) {
    return std::make_shared<RequestLifecycle>(*this, std::move(context));
}

void HttpServer::record_request_start(const RequestLogContext& context) {
    events_.emit_request_start(context);
    operational_log_.request_start(context);
}

void HttpServer::record_request_rejected(const RequestRejectionLogContext& context) {
    events_.emit_request_rejected(context);
    operational_log_.request_rejected(context);
}

void HttpServer::record_request_done(const RequestLogContext& context,
                                     const GenerationOutcome& outcome) {
    events_.emit_request_done(context, outcome);
    metrics_.record(outcome);
    operational_log_.request_done(context, outcome);
}

void HttpServer::record_request_failure(const RequestLogContext& context,
                                        const RequestFailure& failure) {
    events_.emit_request_error(context, failure.machine_message);
    operational_log_.request_failure(context, failure);
}

void HttpServer::record_response_failure(std::uint64_t request_id, const RequestFailure& failure) {
    operational_log_.response_failure(request_id, failure);
}

void HttpServer::record_throughput(const ThroughputReport& report) {
    events_.emit_throughput(report);
    operational_log_.throughput(report);
}

void HttpServer::run_stats_reporter() {
    using Clock                     = std::chrono::steady_clock;
    ninfer::RuntimeStats previous   = service_->runtime_stats();
    Clock::time_point previous_time = Clock::now();
    const auto interval             = std::chrono::milliseconds(options_.log_stats_interval_ms);
    Clock::time_point next_deadline = previous_time + interval;

    for (;;) {
        {
            std::unique_lock lock(stats_mutex_);
            if (stats_cv_.wait_until(lock, next_deadline, [this] { return stats_stopping_; })) {
                break;
            }
        }

        const ninfer::RuntimeStats current = service_->runtime_stats();
        const Clock::time_point now        = Clock::now();
        const ThroughputReport report      = make_throughput_report(
            previous, current, std::chrono::duration<double>(now - previous_time).count());
        if (report_has_activity(report)) { record_throughput(report); }
        previous      = current;
        previous_time = now;
        next_deadline += interval;
        const Clock::time_point after_write = Clock::now();
        if (next_deadline <= after_write) { next_deadline = after_write + interval; }
    }

    const ninfer::RuntimeStats current = service_->runtime_stats();
    const Clock::time_point now        = Clock::now();
    const ThroughputReport tail        = make_throughput_report(
        previous, current, std::chrono::duration<double>(now - previous_time).count());
    // The exact partial interval remains useful to measurement consumers. Pretty throughput is a
    // fixed-cadence operational record and deliberately has no irregular shutdown tail.
    if (report_has_activity(tail)) { events_.emit_throughput(tail); }
}

void HttpServer::stop_stats_reporter() {
    if (!stats_thread_.joinable()) { return; }
    {
        std::lock_guard lock(stats_mutex_);
        stats_stopping_ = true;
    }
    stats_cv_.notify_one();
    stats_thread_.join();
}

void HttpServer::register_routes() {
    server_.set_error_handler([this](const httplib::Request& request, httplib::Response& response) {
        return handle_unrendered_http_error(options_, request, response);
    });
    if (options_.enable_cors) {
        server_.set_default_headers(
            {{"Access-Control-Allow-Origin", "*"},
             {"Access-Control-Expose-Headers", "x-request-id, request-id"},
             {"Access-Control-Allow-Headers",
              "Authorization, Content-Type, X-API-Key, anthropic-version, anthropic-beta, "
              "anthropic-user-profile-id"},
             {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"}});
        // CORS preflight: browsers send OPTIONS with no credentials before the real
        // request; answer it without auth so the actual GET/POST can carry the key.
        server_.Options(R"(.*)",
                        [](const httplib::Request&, httplib::Response& res) { res.status = 204; });
    }

    server_.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        ensure_openai_request_id(req, res);
        if (options_.api_key.empty() || req.path == "/health" || req.method == "OPTIONS") {
            return httplib::Server::HandlerResponse::Unhandled;
        }
        // Accept both the OpenAI-style bearer token and the Anthropic-style
        // x-api-key header so OpenAI clients and Claude Code (ANTHROPIC_API_KEY
        // -> x-api-key, ANTHROPIC_AUTH_TOKEN -> Authorization: Bearer) both work.
        const bool bearer_ok =
            matches_bearer_credential(req.get_header_value("Authorization"), options_.api_key);
        const bool x_api_key_ok = req.get_header_value("x-api-key") == options_.api_key;
        if (!bearer_ok && !x_api_key_ok) {
            ApiError error;
            error.status  = 401;
            error.type    = "invalid_request_error";
            error.code    = "invalid_api_key";
            error.message = "missing or invalid API key";
            // Render the 401 in the shape the target endpoint speaks.
            if (req.path.rfind("/v1/messages", 0) == 0) {
                write_anthropic_error(res, error, new_anthropic_request_id());
            } else {
                write_openai_error(res, error);
            }
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    server_.set_exception_handler(
        [this](const httplib::Request& req, httplib::Response& res, std::exception_ptr ep) {
            ensure_openai_request_id(req, res);
            try {
                std::rethrow_exception(ep);
            } catch (const ApiException& e) {
                if (e.error().status >= 500) {
                    operational_log_.http_failure(
                        endpoint_name(req.path),
                        make_request_failure(RequestFailurePhase::Http, e.error()),
                        response_request_id(res));
                }
                if (req.path.rfind("/v1/messages", 0) == 0) {
                    write_anthropic_error(res, e.error(), new_anthropic_request_id());
                } else {
                    write_openai_error(res, e.error());
                }
            } catch (const std::exception& e) {
                operational_log_.http_failure(
                    endpoint_name(req.path),
                    make_internal_request_failure(RequestFailurePhase::Http, e.what()),
                    response_request_id(res));
                if (req.path.rfind("/v1/messages", 0) == 0) {
                    ApiError error;
                    error.status  = 500;
                    error.message = e.what();
                    write_anthropic_error(res, error, new_anthropic_request_id());
                } else {
                    write_exception(res, e);
                }
            } catch (...) {
                operational_log_.http_failure(
                    endpoint_name(req.path),
                    make_internal_request_failure(RequestFailurePhase::Http, "unknown error"),
                    response_request_id(res));
                ApiError error;
                error.status  = 500;
                error.type    = "internal_error";
                error.message = "unknown error";
                if (req.path.rfind("/v1/messages", 0) == 0) {
                    write_anthropic_error(res, error, new_anthropic_request_id());
                } else {
                    write_openai_error(res, error);
                }
            }
        });

    // Readiness and liveness in one answer. Before the service attaches (model still loading)
    // and after a latched engine failure the server answers 503, so a supervisor, load balancer
    // or fleet dashboard never routes to an instance that cannot serve. A latched failure is
    // permanent - every request then returns 503 "inference engine is unavailable" and only a
    // restart recovers - so a hardcoded ok here would hide exactly that state.
    server_.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        const bool available =
            service_ != nullptr && service_->is_available() && service_->healthy();
        res.status = available ? 200 : 503;
        res.set_content(nlohmann::json{{"status", available ? "ok" : "unavailable"}}.dump(),
                        "application/json");
    });
    // One complete live snapshot for the dashboard: board telemetry, the scheduler's own
    // occupancy view, the VRAM budget, and the context cache's occupancy against its configured
    // capacity. Every field here is either absent from /metrics or only derivable there by
    // differencing counters; the capacity denominators exist nowhere else on the wire.
    server_.Get("/telemetry", [this](const httplib::Request& req, httplib::Response& res) {
        handle_telemetry(req, res);
    });
    // The request-log record stream - byte-identical to what --request-log-jsonl appends - as
    // named SSE events. A new reader is replayed the retained server_start record and the recent
    // ring before live delivery begins.
    server_.Get("/events", [this](const httplib::Request& req, httplib::Response& res) {
        handle_events(req, res);
    });
    server_.Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(metrics_.render(options_.max_concurrency,
                                        service_ != nullptr ? service_->runtime_stats()
                                                            : ninfer::RuntimeStats{},
                                        service_ != nullptr ? service_->active_request_count() : 0),
                        "text/plain; version=0.0.4");
    });
    // llama.cpp-shaped slot detail, read from the Engine's continuation catalog: one slot per
    // private catalog cell. A cell claimed by a running request reports that request's prompt
    // and reused-prefix sizes; a retained cell reports the resident session's depth (as both
    // tokens and cache, matching llama.cpp's retained slot) plus its identifying
    // `session_digest`. Before the service attaches (model still loading) every slot reads
    // idle.
    server_.Get("/slots", [this](const httplib::Request&, httplib::Response& res) {
        const bool speculative =
            options_.speculative.backend != ninfer::SpeculativeBackend::None;
        std::vector<ninfer::SlotState> states;
        std::uint32_t slot_count = options_.max_concurrency;
        if (service_ != nullptr) {
            states     = service_->slot_states();
            slot_count = service_->slot_count();
        }
        nlohmann::json slots = nlohmann::json::array();
        for (std::uint32_t i = 0; i < slot_count; ++i) {
            const ninfer::SlotState state =
                i < states.size() ? states[i] : ninfer::SlotState{};
            nlohmann::json checkpoints = nlohmann::json::array();
            for (const ninfer::SlotCheckpoint& checkpoint : state.checkpoints) {
                checkpoints.push_back({{"frontier", checkpoint.frontier},
                                       {"session_digest", checkpoint.session_digest}});
            }
            slots.push_back({{"id", i},
                             {"is_processing", state.processing},
                             {"retained", state.retained},
                             {"session_digest", state.session_digest},
                             {"checkpoints", std::move(checkpoints)},
                             {"n_ctx", options_.max_context},
                             {"n_prompt_tokens", state.prompt_tokens},
                             {"n_prompt_tokens_cache", state.cached_tokens},
                             {"speculative", speculative}});
        }
        res.set_content(slots.dump(), "application/json");
    });
    // llama.cpp-shaped session persistence: POST /slots/{id}?action=save|restore|erase with
    // {"filename": NAME}. Enabled only by --slot-save-path.
    server_.Post(R"(/slots/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_slot_action(req, res);
    });
    server_.Get("/v1/models", [this](const httplib::Request& req, httplib::Response& res) {
        handle_models(req, res);
    });
    server_.Get(R"(/v1/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model(req, res);
    });
    server_.Post("/v1/chat/completions",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     handle_chat_completions(req, res);
                 });
    server_.Post("/v1/responses", [this](const httplib::Request& req, httplib::Response& res) {
        handle_responses(req, res);
    });
    server_.Post("/v1/responses/input_tokens",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     handle_response_input_tokens(req, res);
                 });
    server_.Post("/v1/responses/compact",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     handle_response_compact(req, res);
                 });
    server_.Post(R"(/v1/responses/([^/]+)/cancel)",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     handle_response_cancel(req, res);
                 });
    server_.Get(R"(/v1/responses/([^/]+)/input_items)",
                [this](const httplib::Request& req, httplib::Response& res) {
                    handle_response_input_items(req, res);
                });
    server_.Get(R"(/v1/responses/([^/]+))",
                [this](const httplib::Request& req, httplib::Response& res) {
                    handle_response_get(req, res);
                });
    server_.Delete(R"(/v1/responses/([^/]+))",
                   [this](const httplib::Request& req, httplib::Response& res) {
                       handle_response_delete(req, res);
                   });
    server_.Post("/v1/messages/count_tokens",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     handle_count_tokens(req, res);
                 });
    server_.Post("/v1/messages", [this](const httplib::Request& req, httplib::Response& res) {
        handle_messages(req, res);
    });
}

void HttpServer::handle_telemetry(const httplib::Request&, httplib::Response& res) const {
    const ninfer::RuntimeStats stats =
        service_ != nullptr ? service_->runtime_stats() : ninfer::RuntimeStats{};
    const ninfer::MemorySummary memory =
        service_ != nullptr ? service_->memory_summary() : ninfer::MemorySummary{};

    nlohmann::json host_work = {
        {"engine_boundary_ns", stats.host_work.engine_boundary_ns},
        {"program_submit_ns", stats.host_work.program_submit_ns},
        {"program_post_ns", stats.host_work.program_post_ns},
        {"engine_commit_output_ns", stats.host_work.engine_commit_output_ns},
        {"engine_maintenance_ns", stats.host_work.engine_maintenance_ns},
        {"device_wait_ns", stats.host_work.device_wait_ns},
        {"decode_host_ns", stats.host_work.decode_host_ns},
        {"decode_device_wait_ns", stats.host_work.decode_device_wait_ns},
        {"prefill_host_ns", stats.host_work.prefill_host_ns},
        {"prefill_device_wait_ns", stats.host_work.prefill_device_wait_ns},
        {"control_host_ns", stats.host_work.control_host_ns},
        {"control_device_wait_ns", stats.host_work.control_device_wait_ns},
        {"prefill_units", stats.host_work.prefill_units},
        {"control_units", stats.host_work.control_units},
        {"admission_policy_ns", stats.host_work.admission_policy_ns},
        {"admission_policy_invocations", stats.host_work.admission_policy_invocations},
        {"context_progress_ns", stats.host_work.context_progress_ns},
        {"context_progress_invocations", stats.host_work.context_progress_invocations},
        {"stats_publication_ns", stats.host_work.stats_publication_ns},
        {"stats_publication_invocations", stats.host_work.stats_publication_invocations}};

    nlohmann::json scheduler = {
        {"running", stats.running_requests},
        {"prefilling", stats.prefilling_requests},
        {"decode_ready", stats.decode_ready_requests},
        {"waiting", stats.waiting_requests},
        {"materializing", stats.materializing_requests},
        {"capture_pending", stats.capture_pending_requests},
        {"terminal_pending", stats.terminal_pending_requests},
        {"max_concurrency", options_.max_concurrency},
        {"max_pending_requests", options_.max_pending_requests},
        {"active_captures_completed", stats.active_captures_completed},
        {"active_captures_aborted", stats.active_captures_aborted},
        {"decode_rounds", stats.decode_rounds},
        {"decode_row_rounds", stats.decode_row_rounds},
        {"decode_rounds_abandoned", stats.decode_rounds_abandoned},
        {"computed_prefill_tokens", stats.computed_prefill_tokens},
        {"committed_decode_tokens", stats.committed_decode_tokens},
        {"prefill_seconds_total", stats.prefill_seconds_total},
        {"decode_seconds_total", stats.decode_seconds_total},
        {"host_work", std::move(host_work)}};

    // Occupancy paired with the capacity it is measured against. A byte count alone cannot say
    // whether the cache is healthy or saturated, and the configured budget reaches the wire
    // nowhere else.
    nlohmann::json cache = {
        {"kv_capacity", memory.kv_capacity},
        {"kv_capacity_page_groups", memory.kv_capacity_page_groups},
        {"kv_capacity_max_page_groups", memory.kv_capacity_max_page_groups},
        {"device_main_kv_occupied_pages", stats.device_main_kv_occupied_pages},
        {"device_backend_kv_occupied_pages", stats.device_backend_kv_occupied_pages},
        {"device_state_occupied_slots", stats.device_state_occupied_slots},
        {"host_state_occupied_slots", stats.host_state_occupied_slots},
        {"host_state_capacity_slots", memory.host_state_capacity_slots},
        {"host_kv_occupied_bytes", stats.host_kv_occupied_bytes},
        {"host_kv_capacity_bytes", memory.host_kv_capacity_bytes},
        {"pressure",
         {{"spill_pages", stats.pressure_spill_pages},
          {"partial_tail_cow_pages", stats.partial_tail_cow_pages},
          {"private_owners_degraded", stats.pressure_private_owners_degraded},
          {"private_owners_evicted", stats.pressure_private_owners_evicted},
          {"shared_owners_degraded", stats.pressure_shared_owners_degraded},
          {"shared_owners_evicted", stats.pressure_shared_owners_evicted},
          {"checkpoints_dropped", stats.pressure_checkpoints_dropped},
          {"searches", stats.pressure_searches},
          {"search_budget_exhaustions", stats.pressure_search_budget_exhaustions},
          {"maximal_fallback_selections", stats.pressure_maximal_fallback_selections}}},
        {"shared_active_references", stats.shared_active_references},
        {"historical_fork_hits", stats.historical_fork_hits},
        {"state",
         {{"moves", stats.state_moves},
          {"forks", stats.state_forks},
          {"restores", stats.state_restores},
          {"d2h",
           {{"count", stats.state_d2h_count}, {"bytes", stats.state_d2h_bytes},
            {"seconds", stats.state_d2h_seconds}}},
          {"h2d",
           {{"count", stats.state_h2d_count}, {"bytes", stats.state_h2d_bytes},
            {"seconds", stats.state_h2d_seconds}}},
          {"d2d",
           {{"count", stats.state_d2d_count}, {"bytes", stats.state_d2d_bytes},
            {"seconds", stats.state_d2d_seconds}}}}},
        {"main_kv",
         {{"d2h",
           {{"pages", stats.main_kv_d2h_pages}, {"bytes", stats.main_kv_d2h_bytes},
            {"seconds", stats.main_kv_d2h_seconds}}},
          {"h2d",
           {{"pages", stats.main_kv_h2d_pages}, {"bytes", stats.main_kv_h2d_bytes},
            {"seconds", stats.main_kv_h2d_seconds}}},
          {"d2d",
           {{"pages", stats.main_kv_d2d_pages}, {"bytes", stats.main_kv_d2d_bytes},
            {"seconds", stats.main_kv_d2d_seconds}}}}},
        {"backend_kv",
         {{"d2h",
           {{"pages", stats.backend_kv_d2h_pages}, {"bytes", stats.backend_kv_d2h_bytes},
            {"seconds", stats.backend_kv_d2h_seconds}}},
          {"h2d",
           {{"pages", stats.backend_kv_h2d_pages}, {"bytes", stats.backend_kv_h2d_bytes},
            {"seconds", stats.backend_kv_h2d_seconds}}},
          {"d2d",
           {{"pages", stats.backend_kv_d2d_pages}, {"bytes", stats.backend_kv_d2d_bytes},
            {"seconds", stats.backend_kv_d2d_seconds}}}}},
        {"actual_context_transfer_seconds", stats.actual_context_transfer_seconds}};

    nlohmann::json slots = nlohmann::json::array();
    if (service_ != nullptr) {
        for (const ninfer::SlotState& state : service_->slot_states()) {
            slots.push_back({{"processing", state.processing},
                             {"retained", state.retained},
                             {"prompt_tokens", state.prompt_tokens},
                             {"cached_tokens", state.cached_tokens},
                             {"session_digest", state.session_digest},
                             {"checkpoints", state.checkpoints.size()}});
        }
    }

    nlohmann::json memory_json = {
        {"device", memory.device},
        {"max_context", memory.max_context},
        {"kv_cache", kv_cache_name(memory.kv_cache)},
        {"kv_capacity", memory.kv_capacity},
        {"kv_capacity_page_groups", memory.kv_capacity_page_groups},
        {"kv_capacity_max_page_groups", memory.kv_capacity_max_page_groups},
        {"weights", arena_json(memory.weights)},
        {"sequence", arena_json(memory.sequence)},
        {"workspace", arena_json(memory.workspace)},
        {"minimum_runtime_reservation_bytes", memory.minimum_runtime_reservation_bytes},
        {"kv_capacity_increment_bytes", memory.kv_capacity_increment_bytes},
        {"runtime_reservation_bytes", memory.runtime_reservation_bytes},
        {"kv_capacity_headroom_bytes", memory.kv_capacity_headroom_bytes},
        {"planned_slack_bytes", memory.planned_slack_bytes},
        {"workspace_logical_peak_bytes", memory.workspace_logical_peak_bytes},
        {"cuda_graph_allowance_bytes", memory.cuda_graph_allowance_bytes},
        {"kv_payload_bytes", memory.kv_payload_bytes},
        {"text_kv_bytes", memory.text_kv_bytes},
        {"mtp_kv_bytes", memory.mtp_kv_bytes},
        {"gdn_state_bytes", memory.gdn_state_bytes},
        {"dflash_kv_bytes", memory.dflash_kv_bytes},
        {"replay_records_bytes", memory.replay_records_bytes},
        {"available_after_weights_bytes", memory.available_after_weights_bytes},
        {"available_after_startup_bytes", memory.available_after_startup_bytes},
        {"host_state_capacity_slots", memory.host_state_capacity_slots},
        {"host_state_occupied_slots", memory.host_state_occupied_slots},
        {"host_kv_capacity_bytes", memory.host_kv_capacity_bytes},
        {"host_kv_occupied_bytes", memory.host_kv_occupied_bytes}};
    if (memory.vision_workspace.has_value()) {
        const auto& vision = *memory.vision_workspace;
        memory_json["vision_workspace"] = {
            {"aggregate_prompt_tokens", vision.aggregate_prompt_tokens},
            {"max_item_tokens", vision.max_item_tokens},
            {"general_capacity_bytes", vision.general_capacity_bytes},
            {"encode_peak_bytes", vision.encode_peak_bytes},
            {"handoff_offset_bytes", vision.handoff_offset_bytes},
            {"handoff_capacity_bytes", vision.handoff_capacity_bytes},
            {"handoff_active_bytes", vision.handoff_active_bytes},
            {"handoff_peak_bytes", vision.handoff_peak_bytes}};
    }

    const nlohmann::json payload = {
        {"timestamp_unix_ms", unix_time_ms()},
        {"server_instance_id", events_.server_instance_id()},
        {"uptime_seconds",
         std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at_).count()},
        {"attached", service_ != nullptr},
        {"model_id", public_model_id_},
        {"gpu", gpu_json(gpu_.read())},
        {"scheduler", std::move(scheduler)},
        {"cache", std::move(cache)},
        {"slots", std::move(slots)},
        {"memory", std::move(memory_json)},
        {"events", {{"jsonl_enabled", events_.jsonl_enabled()},
                    {"subscribers", events_.subscriber_count()}}}};
    res.set_content(payload.dump(), "application/json");
}

void HttpServer::handle_events(const httplib::Request&, httplib::Response& res) {
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");

    std::vector<std::string> backlog;
    std::shared_ptr<EventSubscriber> subscriber = events_.subscribe(backlog);

    res.set_chunked_content_provider(
        "text/event-stream",
        [this, subscriber, backlog = std::move(backlog)](std::size_t offset,
                                                         httplib::DataSink& sink) mutable {
            if (offset == 0) {
                // SSE retry hint plus the retained opening state, so a reconnecting dashboard is
                // immediately consistent without a separate bootstrap request.
                std::string opening = "retry: 2000\n\n";
                for (const std::string& record : backlog) {
                    opening += event_frame(record_event_name(record), record);
                }
                backlog.clear();
                if (!sink.write(opening.data(), opening.size())) { return false; }
            }
            std::string record;
            if (subscriber->next(record, kEventKeepalive)) {
                const std::string frame = event_frame(record_event_name(record), record);
                return sink.write(frame.data(), frame.size());
            }
            static constexpr std::string_view kKeepalive = ": keepalive\n\n";
            return sink.write(kKeepalive.data(), kKeepalive.size());
        },
        [this, subscriber](bool) { events_.unsubscribe(subscriber); });
}

void HttpServer::handle_models(const httplib::Request&, httplib::Response& res) const {
    res.set_content(make_models_list(public_model_id_, unix_time_now(), options_.max_context,
                                     options_.enable_vision),
                    "application/json");
}

void HttpServer::handle_model(const httplib::Request& req, httplib::Response& res) const {
    const std::string id = req.matches.size() > 1 ? req.matches[1].str() : std::string();
    if (id != public_model_id_) {
        ApiError error;
        error.status  = 404;
        error.type    = "invalid_request_error";
        error.code    = "model_not_found";
        error.message = "model '" + id + "' not found";
        write_openai_error(res, error);
        return;
    }
    res.set_content(make_model_object(public_model_id_, unix_time_now(), options_.max_context,
                                      options_.enable_vision),
                    "application/json");
}

void HttpServer::handle_slot_action(const httplib::Request& req, httplib::Response& res) {
    const auto fail = [&res](int status, std::string code, std::string message) {
        ApiError error;
        error.status  = status;
        error.type    = status >= 500 ? "server_error" : "invalid_request_error";
        error.code    = std::move(code);
        error.message = std::move(message);
        write_openai_error(res, error);
    };
    if (options_.slot_save_path.empty()) {
        fail(501, "slot_persistence_disabled",
             "this server was started without --slot-save-path; slot save/restore is disabled");
        return;
    }
    const std::string id_text = req.matches.size() > 1 ? req.matches[1].str() : std::string();
    std::uint32_t slot        = 0;
    try {
        slot = static_cast<std::uint32_t>(std::stoul(id_text));
    } catch (const std::exception&) {
        fail(400, "invalid_slot", "slot id is not a number");
        return;
    }
    const std::uint32_t slot_count =
        service_ != nullptr ? service_->slot_count() : options_.max_concurrency;
    if (slot >= slot_count) {
        fail(400, "invalid_slot",
             "slot " + id_text + " is outside this server's " + std::to_string(slot_count) +
                 " slots");
        return;
    }
    const std::string action = req.get_param_value("action");

    // Body: {"filename": NAME} for save/restore, plus optional {"if_digest": DIGEST} on save
    // and erase - a precondition that the slot still holds the session the client means,
    // checked atomically with the operation (mismatch = 409 slot_session_mismatch).
    std::string filename;
    std::string if_digest;
    try {
        const nlohmann::json body =
            req.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body);
        filename  = body.value("filename", std::string());
        if_digest = body.value("if_digest", std::string());
    } catch (const std::exception&) {
        fail(400, "invalid_request", "request body is not valid JSON");
        return;
    }

    if (action == "erase") {
        try {
            const std::uint32_t erased = service_->slot_erase(slot, if_digest);
            logger_->info("{}", "slot erase id=" + id_text + " n_erased=" + std::to_string(erased));
            res.set_content(nlohmann::json{{"id_slot", slot}, {"n_erased", erased}}.dump(),
                            "application/json");
        } catch (const ninfer::RequestError& engine_error) {
            fail(409, "slot_busy", engine_error.what());
        } catch (const ninfer::SlotSessionMismatch& mismatch) {
            fail(409, "slot_session_mismatch", mismatch.what());
        }
        return;
    }
    if (action != "save" && action != "restore") {
        fail(400, "invalid_action", "action must be save, restore, or erase");
        return;
    }
    const std::optional<std::string> sanitized = sanitize_slot_filename(filename);
    if (!sanitized) {
        fail(400, "invalid_filename",
             "filename must be 1-" + std::to_string(kSlotFilenameMaxBytes) +
                 " chars of [A-Za-z0-9._-] and must not start with a dot");
        return;
    }
    const std::string path = options_.slot_save_path + "/" + *sanitized;

    try {
        if (action == "save") {
            const ninfer::SlotSaveResult saved = service_->slot_save(slot, path, if_digest);
            logger_->info("{}", "slot save id=" + id_text + " file=" + *sanitized +
                     " n_saved=" + std::to_string(saved.tokens) +
                     " n_written=" + std::to_string(saved.bytes) +
                     " session=" + saved.session_digest + " in " +
                     format_seconds(saved.seconds) + " s");
            res.set_content(
                nlohmann::json{{"id_slot", slot},
                               {"filename", *sanitized},
                               {"n_saved", saved.tokens},
                               {"n_written", saved.bytes},
                               {"session_digest", saved.session_digest},
                               {"timings", {{"save_ms", saved.seconds * 1000.0}}}}
                    .dump(),
                "application/json");
        } else {
            const ninfer::SlotRestoreResult restored = service_->slot_restore(slot, path);
            logger_->info("{}", "slot restore id=" + id_text + " file=" + *sanitized +
                     " n_restored=" + std::to_string(restored.tokens) +
                     " n_read=" + std::to_string(restored.bytes) +
                     " session=" + restored.session_digest + " in " +
                     format_seconds(restored.seconds) + " s");
            res.set_content(
                nlohmann::json{{"id_slot", slot},
                               {"filename", *sanitized},
                               {"n_restored", restored.tokens},
                               {"n_read", restored.bytes},
                               {"session_digest", restored.session_digest},
                               {"timings", {{"restore_ms", restored.seconds * 1000.0}}}}
                    .dump(),
                "application/json");
        }
    } catch (const ninfer::RequestError& engine_error) {
        fail(409, "slot_busy", engine_error.what());
    } catch (const ninfer::SlotSessionMismatch& mismatch) {
        fail(409, "slot_session_mismatch", mismatch.what());
    } catch (const std::invalid_argument& engine_error) {
        fail(400, "slot_" + action + "_failed", engine_error.what());
    }
}

bool HttpServer::bind() { return server_.bind_to_port(options_.host, options_.port); }

void HttpServer::attach(GenerationService& service) {
    if (service_ != nullptr) {
        throw std::logic_error("HTTP generation service is already attached");
    }
    const ninfer::LoadSummary load = service.load_summary();
    public_model_id_               = resolve_public_model_id(options_, load.model_id);
    service_                       = &service;
    events_.emit_server_start(options_, service.engine_options(), service.sampling_defaults(),
                              public_model_id_, load, service.memory_summary());
}

bool HttpServer::listen() {
    if (service_ == nullptr) { throw std::logic_error("HTTP generation service is not attached"); }
    if (public_model_id_.empty()) {
        throw std::logic_error("HTTP public model id is not resolved");
    }
    if (options_.log_stats_interval_ms != 0) {
        stats_stopping_ = false;
        stats_thread_   = std::thread([this] { run_stats_reporter(); });
    }
    try {
        const bool result = server_.listen_after_bind();
        stop_stats_reporter();
        return result;
    } catch (...) {
        stop_stats_reporter();
        throw;
    }
}

void HttpServer::stop() {
    events_.close_all();
    server_.stop();
}

} // namespace ninfer::serve
