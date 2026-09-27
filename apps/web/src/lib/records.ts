// Record shapes, as emitted by src/serve/request_log.cpp (schema 21).
//
// GET /events streams these live and `--request-log-jsonl` appends the identical lines, so one
// set of types serves both the live dashboard and file replay. Only the fields the dashboard
// reads are declared; a record carries more, and unread fields are deliberately not mirrored
// here because every declaration is a second place to keep in step with the C++ formatter.
//
// Records are discriminated on `event`, never on `schema_version`, so an older log still
// replays: fields added by a later schema are declared optional and read as absent.

export interface RequestContext {
  request_id: number
  protocol: string
  model: string
  stream: boolean
  message_count: number
  media_item_count: number
  requested_output_tokens: number
  tool_count: number
  tool_choice: string
  has_tool_history: boolean
  enable_thinking: boolean
  thinking_budget: number | null
  preserve_thinking: boolean
  sampling: {
    temperature: number
    top_p: number
    top_k: number | null
    min_p: number
    presence_penalty: number
    frequency_penalty: number
    seed: number | null
  }
}

/** Per-request phase durations. `ttft` is the quantity the queue/prefill split explains. */
export interface RequestTimings {
  prepare: number
  ttft: number
  vision: number
  prefill: number
  decode: number
  total: number
}

export interface SpeculativeStats {
  backend: string
  draft_window: number
  rounds: number
  drafted_tokens: number
  accepted_tokens: number
  fallback_steps: number
  /** Acceptance count per draft position; its decay is the shape of the MTP accept curve. */
  accepted_per_position: number[]
}

export interface RequestResult {
  finish_reason: string
  prompt_tokens: number
  completion_tokens: number
  /** Prompt suffix tokens actually computed; prefix reuse kept the rest out of prefill. */
  computed_prefill_tokens: number
  prefix_cache_hit_tokens: number
  prefix_reuse_path: string
  tool_call_count: number
}

/**
 * One reporting interval. The reporter only emits when the interval had activity, so samples are
 * irregularly spaced and `interval_seconds` — not the gap between timestamps — is the authority
 * on the window a sample covers.
 */
export interface IntervalEnergy {
  board_joules: number
  prefill_joules: number
  decode_joules: number
  idle_joules: number
  idle_watts: number
  accounted_seconds: number
  residual_joules: number
  /** Share of board joules the phase split and idle baseline fail to account for. */
  residual_fraction: number
  /**
   * Joules per token, null per figure when its token denominator is zero: no tokens is not
   * zero joules each, and the two must not render the same.
   */
  joules_per_token: {
    served: number | null
    active: number | null
    prefill: number | null
    decode: number | null
  }
}

interface RecordEnvelope {
  schema_version: number
  server_instance_id: string
  timestamp_unix_ms: number
  artifact_type: string
}

export interface ServerStartRecord extends RecordEnvelope {
  event: 'server_start'
  argv: string[]
  server: {
    host: string
    port: number
    public_model_id: string
    api_key_configured: boolean
    cors_enabled: boolean
    max_request_bytes: number
    request_log_jsonl: string
    slot_save_path: string
    default_output_tokens: number
    default_thinking: boolean
  }
  artifact: {
    path: string
    size_bytes: number | null
    target: string
    weights_id: string
    bytes_read: number
    host_to_device_bytes: number
    peak_staging_bytes: number
    tensor_count: number
    resource_count: number
    load_seconds: number
    upload_seconds: number
  }
  engine: {
    device: number
    max_context: number
    kv_capacity_mode: string
    kv_capacity: number
    kv_capacity_page_groups: number
    kv_capacity_max_page_groups: number
    max_concurrency: number
    max_pending_requests: number
    pending_timeout_ms: number
    prefill_chunk: number
    log_stats_interval_ms: number
    kv_cache: string
    vision: boolean
    cuda_graph: boolean
    prefix_reuse: boolean
    speculative_backend: string
    speculative_draft_window: number
    context_cost: {
      transfer_source: string
      prefill_source: string
      hardware_class: string
      model_id: string
      weights_id: string
      preset_path: string
    }
    context_cache: {
      enabled: boolean
      device_state_slots: number
      total_device_state_slots: number
      host_state_slots: number
      host_kv_capacity_bytes: number
      max_private_continuations: number
      max_shared_prefixes: number
      max_long_anchors_per_continuation: number
    }
  }
  memory: {
    weights: { capacity_bytes: number; used_bytes: number; peak_used_bytes: number }
    sequence: { capacity_bytes: number; used_bytes: number; peak_used_bytes: number }
    workspace: { capacity_bytes: number; used_bytes: number; peak_used_bytes: number }
    minimum_runtime_reservation_bytes: number
    kv_capacity_increment_bytes: number
    runtime_reservation_bytes: number
    available_after_weights_bytes: number
    available_after_startup_bytes: number
    kv_capacity_headroom_bytes: number
    planned_slack_bytes: number
    cuda_graph_allowance_bytes: number
    kv_payload_bytes: number
    host_state_capacity_slots: number
    host_state_occupied_slots: number
    host_kv_capacity_bytes: number
    host_kv_occupied_bytes: number
  }
  environment: {
    device: number
    gpu_name: string
    gpu_uuid: string
    total_device_memory_bytes: number
    compute_capability_major: number
    compute_capability_minor: number
    cuda_compile_version: string
    cuda_runtime_version: string
    cuda_driver_version: string
  }
}

export interface RequestStartRecord extends RecordEnvelope {
  event: 'request_start'
  request: RequestContext
  preparation_seconds: {
    total: number
    acquisition: number
    tokenize: number
    media_items: number
    media_bytes: number
    vision_tokens: number
    cache_hits: number
    cache_misses: number
  }
}

export interface RequestDoneRecord extends RecordEnvelope {
  event: 'request_done'
  request: RequestContext
  result: RequestResult
  timings_seconds: RequestTimings
  engine_timing: {
    queue_wait_seconds: number
    host_exposed_seconds: {
      engine_boundary: number
      program_submit: number
      program_post: number
      engine_commit_output: number
      engine_maintenance: number
      total: number
    }
    device_wait_exposed_seconds: number
    decode: { host_exposed_seconds: number; device_wait_exposed_seconds: number; rounds: number }
    units: { prefill: number; control: number }
  }
  speculative: SpeculativeStats | null
  materialization: {
    predicted_now_ns: number
    predicted_total_ns: number
    planning_elapsed_ns: number
    search_elapsed_ns: number
    stop_reason: string
  }
}

export interface RequestErrorRecord extends RecordEnvelope {
  event: 'request_error'
  request: RequestContext
  error: { message: string }
}

export interface ThroughputRecord extends RecordEnvelope {
  event: 'throughput'
  interval_seconds: number
  tokens: { computed_prefill: number; committed_decode: number }
  throughput_tokens_per_second: { prefill: number; decode: number }
  /**
   * Board energy for the interval, or null on boards without a cumulative energy counter:
   * "could not measure" must not render as "used none".
   */
  energy: IntervalEnergy | null
  scheduler: {
    running: number
    prefilling: number
    decode_ready: number
    waiting: number
    materializing: number
    capture_pending: number
    terminal_pending: number
  }
  decode_batch: { rounds: number; row_rounds: number; average_size: number | null }
  host_work: {
    elapsed_seconds: {
      engine_boundary: number
      program_submit: number
      program_post: number
      engine_commit_output: number
      engine_maintenance: number
      total: number
    }
    device_wait_seconds: number
    work_class_seconds: {
      decode_host: number
      decode_device_wait: number
      prefill_host: number
      prefill_device_wait: number
      control_host: number
      control_device_wait: number
    }
    detail_subset_seconds: {
      admission_policy: number
      context_progress: number
      stats_publication: number
    }
    detail_invocations: {
      admission_policy: number
      context_progress: number
      stats_publication: number
    }
    units: { prefill: number; control: number }
  }
  context_cache: {
    captures: { completed: number; aborted: number }
    selections: {
      root: number
      private_endpoint: number
      private_turn_closure: number
      private_response_replay: number
      private_long_anchor: number
      shared_stable_prefix: number
      reused_prompt_tokens: number
    }
    last_selection: { frontier_tokens: number }
    state_operations: { moves: number; forks: number; restores: number }
    state_transfers: {
      d2h: { count: number; bytes: number; seconds: number }
      h2d: { count: number; bytes: number; seconds: number }
      d2d: { count: number; bytes: number; seconds: number }
    }
    main_kv_transfers: {
      d2h: { pages: number; bytes: number; seconds: number }
      h2d: { pages: number; bytes: number; seconds: number }
      d2d: { pages: number; bytes: number; seconds: number }
    }
    backend_kv_transfers: {
      d2h: { pages: number; bytes: number; seconds: number }
      h2d: { pages: number; bytes: number; seconds: number }
      d2d: { pages: number; bytes: number; seconds: number }
    }
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
      historical_fork_hits: number
    }
    /** Interval-end occupancy; pair with the server_start capacity to read health. */
    occupancy: {
      device_state_slots: number
      host_state_slots: number
      device_main_kv_pages: number
      device_backend_kv_pages: number
      host_kv_bytes: number
      shared_active_references: number
    }
    actual_transfer_seconds: number
  }
}

export type EngineRecord =
  | ServerStartRecord
  | RequestStartRecord
  | RequestDoneRecord
  | RequestErrorRecord
  | ThroughputRecord

export type RecordEvent = EngineRecord['event']

export function isEngineRecord(value: unknown): value is EngineRecord {
  if (typeof value !== 'object' || value === null) return false
  const candidate = value as Record<string, unknown>
  if (typeof candidate.event !== 'string') return false
  if (typeof candidate.server_instance_id !== 'string') return false
  if (typeof candidate.timestamp_unix_ms !== 'number') return false
  return candidate.event in {
    server_start: true,
    request_start: true,
    request_done: true,
    request_error: true,
    throughput: true,
  }
}

/**
 * Parses one JSONL line. Returns null for blank lines and for a torn trailing record, which a
 * reader tailing a file the server is still appending to will encounter.
 */
export function parseRecordLine(line: string): EngineRecord | null {
  if (line.length === 0) return null
  try {
    const parsed: unknown = JSON.parse(line)
    return isEngineRecord(parsed) ? parsed : null
  } catch {
    return null
  }
}