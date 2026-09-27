#include "serve/serve_metrics.h"

#include <algorithm>
#include <cstdio>

namespace ninfer::serve {

namespace {

void append_counter(std::string& out, const char* name, std::uint64_t value) {
    char line[160];
    std::snprintf(line, sizeof(line), "%s %llu\n", name,
                  static_cast<unsigned long long>(value));
    out += line;
}

void append_counter(std::string& out, const char* name, double value) {
    char line[160];
    std::snprintf(line, sizeof(line), "%s %.6f\n", name, value);
    out += line;
}

} // namespace

void ServeMetrics::record(const GenerationOutcome& outcome) {
    const GenerationMetrics& m = outcome.metrics;
    const std::uint64_t cached = m.prefix_cache_hit_tokens;
    const std::uint64_t prompt = outcome.prompt_tokens > 0
                                     ? static_cast<std::uint64_t>(outcome.prompt_tokens)
                                     : 0;

    const std::lock_guard<std::mutex> lock(mutex_);
    requests_total_ += 1;
    prefix_cache_hit_tokens_total_ += cached;
    speculative_draft_tokens_total_ += m.speculative_draft_tokens;
    speculative_accepted_tokens_total_ += m.speculative_accepted_tokens;
    last_completed_.prompt_tokens = static_cast<int>(prompt);
    // Clamped like computed_prefill above: a cache figure reported larger
    // than the prompt must not advertise more resident tokens than exist.
    last_completed_.cached_tokens = static_cast<int>(std::min(cached, prompt));
}

ServeMetrics::LastCompleted ServeMetrics::last_completed() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return last_completed_;
}

std::string ServeMetrics::render(std::uint32_t max_concurrency, const ninfer::RuntimeStats& live,
                                 std::size_t active_requests,
                                 const ServerEnergyTotals& energy) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t in_flight  = active_requests;
    const std::uint64_t processing = std::min<std::uint64_t>(in_flight, max_concurrency);
    std::string out;
    out.reserve(704);
    append_counter(out, "llamacpp:prompt_tokens_total", live.computed_prefill_tokens);
    append_counter(out, "llamacpp:prompt_seconds_total", live.prefill_seconds_total);
    append_counter(out, "llamacpp:tokens_predicted_total", live.committed_decode_tokens);
    append_counter(out, "llamacpp:tokens_predicted_seconds_total", live.decode_seconds_total);
    append_counter(out, "llamacpp:requests_processing", processing);
    append_counter(out, "llamacpp:requests_deferred", in_flight - processing);
    append_counter(out, "ninfer:requests_total", requests_total_);
    append_counter(out, "ninfer:prefix_cache_hit_tokens_total", prefix_cache_hit_tokens_total_);
    append_counter(out, "ninfer:draft_tokens_total", speculative_draft_tokens_total_);
    append_counter(out, "ninfer:draft_accepted_tokens_total", speculative_accepted_tokens_total_);
    // Energy is exported as counters, never as a joules-per-token gauge. The denominators are
    // already here (llamacpp:prompt_tokens_total and llamacpp:tokens_predicted_total), so a
    // scraper divides two rates over a window it chose; a gauge computed here would fix that
    // window and would average incorrectly when aggregated.
    if (energy.available) {
        append_counter(out, "ninfer:board_energy_joules_total", energy.board_joules_total);
        append_counter(out, "ninfer:board_idle_watts", energy.idle_watts);
        append_counter(out, "ninfer:prefill_energy_joules_total", live.prefill_energy_joules);
        append_counter(out, "ninfer:decode_energy_joules_total", live.decode_energy_joules);
        append_counter(out, "ninfer:energy_accounted_seconds_total",
                       live.energy_accounted_seconds);
        append_counter(out, "ninfer:energy_samples_total", live.energy_samples);
    }
    return out;
}

} // namespace ninfer::serve
