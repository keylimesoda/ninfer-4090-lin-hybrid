#pragma once

// Cumulative counters behind GET /metrics, in the flat `name value` subset of
// the Prometheus text format.
//
// The four llamacpp:-prefixed counters reproduce llama.cpp's --metrics
// semantics - computed prefill tokens (prefix-cache hits excluded) billed
// against prefill unit time, committed decode tokens against decode unit
// time - so scrapers that difference llama.cpp counters read this server
// without changes. They are sourced from the Engine's live per-unit totals,
// so they advance during a request like llama.cpp's do, not only at its
// completion. The ninfer:-prefixed series report what llama.cpp cannot:
// speculative draft/acceptance totals and prefix-cache reuse.

#include "serve/generation_service.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace ninfer::serve {

// Board energy accumulated since this server started, and the idle draw measured while it had
// nothing to run. Both come from the interval reporter, which owns the only affordable place to
// read the board's cumulative energy counter.
//
// Energy since server start rather than since driver load: a counter scoped to the process is what
// a scraper can attribute to this server, and it starts at zero rather than at whatever the board
// had already drawn. Driver-reload resets are dropped by the reporter, never carried through.
struct ServerEnergyTotals {
    bool available             = false;
    double board_joules_total  = 0.0;
    double idle_watts          = 0.0;
};

class ServeMetrics {
public:
    // Accumulates one completed request. Called from the same funnel as the
    // request-done log line, so every protocol and both streaming modes count.
    void record(const GenerationOutcome& outcome);

    // Prompt/cache sizes of the most recent completed request, retained for
    // /slots. llama.cpp keeps the last request's counts on an idle slot and
    // scrapers (the fleet dashboard) read them as the resident session
    // depth; the prefix cache genuinely still holds that session, so the
    // retained figure stays truthful until the next completion replaces it.
    struct LastCompleted {
        int prompt_tokens = 0;
        int cached_tokens = 0;
    };
    [[nodiscard]] LastCompleted last_completed() const;

    // One complete Prometheus text body, without HTTP framing. The supplied
    // request-lifetime count begins before preparation/submission and survives
    // through response release, so every accepted request remains visible.
    // `live` supplies the four llamacpp token/seconds counters from the Engine's per-unit
    // totals, so scrapers see rates advance during a request; the completion-based sums this
    // class accumulates back the ninfer: series and the idle slot display.
    // `energy` is omitted entirely when the board exposes no cumulative counter, so a scraper sees
    // no series rather than a flat zero it would read as a working meter.
    [[nodiscard]] std::string render(std::uint32_t max_concurrency,
                                     const ninfer::RuntimeStats& live, std::size_t active_requests,
                                     const ServerEnergyTotals& energy = {}) const;

private:
    mutable std::mutex mutex_;
    std::uint64_t requests_total_                    = 0;
    std::uint64_t prefix_cache_hit_tokens_total_     = 0;
    std::uint64_t speculative_draft_tokens_total_    = 0;
    std::uint64_t speculative_accepted_tokens_total_ = 0;
    LastCompleted last_completed_;
};

} // namespace ninfer::serve
