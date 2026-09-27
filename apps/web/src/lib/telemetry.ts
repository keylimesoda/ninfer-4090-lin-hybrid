// GET /telemetry payload, as built by HttpServer::handle_telemetry.
//
// This is a complete snapshot rather than a delta, which is what lets the dashboard resynchronize
// after dropped stream records without replaying anything.

import type { ServerStartRecord, ThroughputRecord } from './records'

export interface GpuTelemetry {
  available: boolean
  error?: string
  name?: string
  uuid?: string
  driver_version?: string
  temperature_c?: number
  fan_percent?: number
  /** Driver-averaged over a trailing 1 s window, which is the right statistic at a 1 Hz poll. */
  power_watts?: number
  power_limit_watts?: number
  /** False on boards with no cumulative energy counter; every energy field is then null. */
  energy_available?: boolean
  /** Board energy since driver load. Only meaningful as a difference within one driver session. */
  energy_joules_total?: number | null
  /** Board energy since this server started, accumulated by the interval reporter. */
  server_energy_joules?: number | null
  /** Board draw measured during intervals that ran no execution unit at all. */
  idle_watts?: number | null
  utilization_gpu_percent?: number
  utilization_memory_percent?: number
  sm_clock_mhz?: number
  sm_clock_max_mhz?: number
  memory_clock_mhz?: number
  memory_used_bytes?: number
  memory_total_bytes?: number
  pcie_rx_bytes_per_second?: number
  pcie_tx_bytes_per_second?: number
  throttle_reasons?: string[]
}

// Boards report throttle reasons that are not faults. `gpu_idle` means there is nothing to run,
// and `applications_clocks_setting` is a deliberate cap, so neither indicates a limit on work the
// engine wanted to do.
const BENIGN_THROTTLES: Record<string, true> = {
  gpu_idle: true,
  applications_clocks_setting: true,
}

/** Throttle reasons that actually constrain achievable throughput. */
export function activeThrottles(gpu: GpuTelemetry | undefined): string[] {
  return (gpu?.throttle_reasons ?? []).filter((reason) => !BENIGN_THROTTLES[reason])
}

/** Scheduler occupancy from the Engine's own view, paired with the limits that bound it. */
export interface SchedulerTelemetry {
  running: number
  prefilling: number
  decode_ready: number
  waiting: number
  materializing: number
  capture_pending: number
  terminal_pending: number
  max_concurrency: number
  max_pending_requests: number
  active_captures_completed: number
  active_captures_aborted: number
  decode_rounds: number
  decode_row_rounds: number
  decode_rounds_abandoned: number
  prefill_seconds_total: number
  decode_seconds_total: number
  /** Wall clock of the single execution thread, split by unit. These sum to its busy time. */
  worker_seconds: {
    engine_boundary: number
    program_submit: number
    program_post: number
    engine_commit_output: number
    engine_maintenance: number
    device_wait: number
    decode_host: number
    decode_device_wait: number
    prefill_host: number
    prefill_device_wait: number
    control_host: number
    control_device_wait: number
  }
  worker_units: { prefill: number; control: number }
  /** Slow-path detail, already contained in the top-level phases; never add it to them. */
  worker_detail: {
    admission_policy: { seconds: number; invocations: number }
    context_progress: { seconds: number; invocations: number }
    stats_publication: { seconds: number; invocations: number }
  }
}

/** One memory arena: capacity, current use, and the peak since process start. */
export interface ArenaTelemetry {
  capacity_bytes: number
  used_bytes: number
  peak_used_bytes: number
}

export interface MemoryTelemetry {
  device: number
  max_context: number
  kv_cache: string
  kv_capacity: number
  kv_capacity_page_groups: number
  kv_capacity_max_page_groups: number
  weights: ArenaTelemetry
  sequence: ArenaTelemetry
  workspace: ArenaTelemetry
  minimum_runtime_reservation_bytes: number
  kv_capacity_increment_bytes: number
  runtime_reservation_bytes: number
  kv_capacity_headroom_bytes: number
  planned_slack_bytes: number
  workspace_logical_peak_bytes: number
  cuda_graph_allowance_bytes: number
  kv_payload_bytes: number
  text_kv_bytes: number
  mtp_kv_bytes: number
  gdn_state_bytes: number
  dflash_kv_bytes: number
  replay_records_bytes: number
  available_after_weights_bytes: number
  available_after_startup_bytes: number
  host_state_capacity_slots: number
  host_state_occupied_slots: number
  host_kv_capacity_bytes: number
  host_kv_occupied_bytes: number
}

export interface SlotTelemetry {
  processing: boolean
  retained: boolean
  prompt_tokens: number
  cached_tokens: number
  session_digest: string
  checkpoints: number
}

export interface Telemetry {
  timestamp_unix_ms: number
  server_instance_id: string
  uptime_seconds: number
  attached: boolean
  model_id: string
  gpu: GpuTelemetry
  scheduler: SchedulerTelemetry
  cache: ContextCacheTelemetry
  slots: SlotTelemetry[]
  memory: MemoryTelemetry
  events: { jsonl_enabled: boolean; subscribers: number }
}

/**
 * The context cache, as the Engine reports it: occupancy measured against the configured
 * capacity, the pressure actions it took to stay inside it, and the host/device movement it
 * performed. There is no tier hierarchy: state is either resident on device or not, and a
 * spill or a restore is the whole story.
 */
export interface ContextCacheTelemetry {
  kv_capacity: number
  kv_capacity_page_groups: number
  kv_capacity_max_page_groups: number
  device_main_kv_occupied_pages: number
  device_backend_kv_occupied_pages: number
  device_state_occupied_slots: number
  host_state_occupied_slots: number
  host_state_capacity_slots: number
  host_kv_occupied_bytes: number
  host_kv_capacity_bytes: number
  pressure: {
    spill_pages: number
    partial_tail_cow_pages: number
    private_owners_degraded: number
    private_owners_evicted: number
    shared_owners_degraded: number
    shared_owners_evicted: number
    checkpoints_dropped: number
    searches: number
    search_budget_exhaustions: number
    maximal_fallback_selections: number
  }
  shared_active_references: number
  historical_fork_hits: number
  state: {
    moves: number
    forks: number
    restores: number
    d2h: { count: number; bytes: number; seconds: number }
    h2d: { count: number; bytes: number; seconds: number }
    d2d: { count: number; bytes: number; seconds: number }
  }
  main_kv: {
    d2h: { pages: number; bytes: number; seconds: number }
    h2d: { pages: number; bytes: number; seconds: number }
    d2d: { pages: number; bytes: number; seconds: number }
  }
  backend_kv: {
    d2h: { pages: number; bytes: number; seconds: number }
    h2d: { pages: number; bytes: number; seconds: number }
    d2d: { pages: number; bytes: number; seconds: number }
  }
  actual_context_transfer_seconds: number
}

/** Prometheus text body from GET /metrics, flattened to a name→value map. */
export function parsePrometheus(body: string): Map<string, number> {
  const out = new Map<string, number>()
  for (const line of body.split('\n')) {
    if (line.length === 0 || line.startsWith('#')) continue
    const split = line.lastIndexOf(' ')
    if (split <= 0) continue
    const value = Number(line.slice(split + 1))
    if (Number.isFinite(value)) out.set(line.slice(0, split).trim(), value)
  }
  return out
}

/**
 * Reconstructs the context-cache view from a replayed log.
 *
 * A throughput record carries the same occupancy the live snapshot reports, and the capacities
 * it is measured against come from the log's own `server_start`, so cache health is readable
 * offline — unlike board telemetry and lane occupancy, which are never recorded. The pressure
 * and transfer counters are the interval deltas the record already carries.
 */
export function cacheFromRecords(
  sample: ThroughputRecord | null,
  start: ServerStartRecord | null,
): ContextCacheTelemetry | undefined {
  if (sample === null || start === null) return undefined
  const cache = sample.context_cache
  const configured = start.engine.context_cache
  return {
    kv_capacity: start.engine.kv_capacity,
    kv_capacity_page_groups: start.engine.kv_capacity_page_groups,
    kv_capacity_max_page_groups: start.engine.kv_capacity_max_page_groups,
    device_main_kv_occupied_pages: cache.occupancy.device_main_kv_pages,
    device_backend_kv_occupied_pages: cache.occupancy.device_backend_kv_pages,
    device_state_occupied_slots: cache.occupancy.device_state_slots,
    host_state_occupied_slots: cache.occupancy.host_state_slots,
    host_state_capacity_slots: configured.host_state_slots,
    host_kv_occupied_bytes: cache.occupancy.host_kv_bytes,
    host_kv_capacity_bytes: configured.host_kv_capacity_bytes,
    pressure: {
      spill_pages: cache.pressure.spill_pages,
      partial_tail_cow_pages: cache.pressure.partial_tail_cow_pages,
      private_owners_degraded: cache.pressure.private_owners_degraded,
      private_owners_evicted: cache.pressure.private_owners_evicted,
      shared_owners_degraded: cache.pressure.shared_owners_degraded,
      shared_owners_evicted: cache.pressure.shared_owners_evicted,
      checkpoints_dropped: cache.pressure.checkpoints_dropped,
      searches: cache.pressure.searches,
      search_budget_exhaustions: cache.pressure.search_budget_exhaustions,
      maximal_fallback_selections: cache.pressure.maximal_fallback_selections,
    },
    shared_active_references: cache.occupancy.shared_active_references,
    historical_fork_hits: cache.pressure.historical_fork_hits,
    state: {
      moves: cache.state_operations.moves,
      forks: cache.state_operations.forks,
      restores: cache.state_operations.restores,
      d2h: cache.state_transfers.d2h,
      h2d: cache.state_transfers.h2d,
      d2d: cache.state_transfers.d2d,
    },
    main_kv: cache.main_kv_transfers,
    backend_kv: cache.backend_kv_transfers,
    actual_context_transfer_seconds: cache.actual_transfer_seconds,
  }
}