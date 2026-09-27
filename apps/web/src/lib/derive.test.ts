import { expect, test } from 'bun:test'

import { percentile, summarizeEnergy, summarizePressure, summarizeRequests } from './derive'
import { parseRecordLine, type RequestDoneRecord, type ThroughputRecord } from './records'

test('percentile matches the truncating nearest-rank rule', () => {
  const values = [5, 1, 3, 2, 4]
  expect(percentile(values, 0.5)).toBe(3)
  expect(percentile(values, 0.9)).toBe(5)
  expect(percentile(values, 0)).toBe(1)
  expect(percentile(values, 1)).toBe(5)
  expect(percentile([], 0.5)).toBe(0)
})

function done(
  id: number,
  values: {
    prompt: number
    computed: number
    generated: number
    queue: number
    prefill: number
    decode: number
    ttft: number
    reusePath?: string
    finishReason?: string
    drafted?: number
    accepted?: number
    perPosition?: number[]
    fallbackSteps?: number
  },
): RequestDoneRecord {
  const queue = values.queue
  const prefill = values.prefill
  return {
    event: 'request_done',
    schema_version: 21,
    server_instance_id: 'serve-test-1',
    timestamp_unix_ms: 1_700_000_000_000 + id * 1000,
    artifact_type: 'ninfer_serve_request_log',
    request: {
      request_id: id,
      protocol: 'openai_responses',
      model: 'qwen3.8-27b',
      stream: true,
      message_count: 4,
      media_item_count: 0,
      requested_output_tokens: 4096,
      tool_count: 0,
      tool_choice: 'auto',
      has_tool_history: false,
      enable_thinking: true,
      thinking_budget: null,
      preserve_thinking: false,
      sampling: {
        temperature: 0.7,
        top_p: 1,
        top_k: null,
        min_p: 0,
        presence_penalty: 0,
        frequency_penalty: 0,
        seed: null,
      },
    },
    result: {
      finish_reason: values.finishReason ?? 'stop_token',
      prompt_tokens: values.prompt,
      completion_tokens: values.generated,
      computed_prefill_tokens: values.computed,
      prefix_cache_hit_tokens: values.prompt - values.computed,
      prefix_reuse_path: values.reusePath ?? 'root',
      tool_call_count: 0,
    },
    timings_seconds: {
      prepare: 0,
      ttft: values.ttft,
      vision: 0,
      prefill,
      decode: values.decode,
      total: 0,
    },
    engine_timing: {
      queue_wait_seconds: queue,
      host_exposed_seconds: {
        engine_boundary: 0,
        program_submit: 0,
        program_post: 0,
        engine_commit_output: 0,
        engine_maintenance: 0,
        total: 0,
      },
      device_wait_exposed_seconds: 0,
      decode: { host_exposed_seconds: 0, device_wait_exposed_seconds: 0, rounds: 0 },
      units: { prefill: 0, control: 0 },
    },
    speculative: values.drafted === undefined ? null : {
      backend: 'mtp',
      draft_window: 3,
      rounds: 10,
      drafted_tokens: values.drafted,
      accepted_tokens: values.accepted ?? 0,
      fallback_steps: values.fallbackSteps ?? 0,
      accepted_per_position: values.perPosition ?? [],
    },
    materialization: {
      predicted_now_ns: 0,
      predicted_total_ns: 0,
      planning_elapsed_ns: 0,
      search_elapsed_ns: 0,
      stop_reason: 'no_pressure',
    },
  }
}

test('request summary partitions TTFT between queue and prefill', () => {
  const records = [
    done(1, { prompt: 2000, computed: 2000, generated: 100, queue: 2, prefill: 1, decode: 1, ttft: 3 }),
    done(2, { prompt: 4000, computed: 1000, generated: 500, queue: 4, prefill: 0.5, decode: 2, ttft: 4.5, reusePath: 'shared_stable_prefix' }),
    done(3, { prompt: 1000, computed: 1000, generated: 5000, queue: 4.5, prefill: 1.75, decode: 3, ttft: 6.25, drafted: 150, accepted: 100, perPosition: [40, 35, 25] }),
    done(4, { prompt: 1500, computed: 1000, generated: 200, queue: 1, prefill: 0, decode: 0.001, ttft: 1, reusePath: 'private_long_anchor' }),
    done(5, { prompt: 1000, computed: 300, generated: 300, queue: 3, prefill: 1, decode: 2, ttft: 4, reusePath: 'root' }),
  ]
  const summary = summarizeRequests(records)
  expect(summary.count).toBe(5)
  expect(summary.totalPromptTokens).toBe(9500)
  expect(summary.totalComputedPrefill).toBe(5300)
  expect(summary.prefillAvoided).toBeCloseTo(1 - 5300 / 9500, 12)
  expect(summary.generated.max).toBe(5000)
  expect(summary.overLongGenerations).toBe(1)
  expect(summary.byReusePath).toEqual({
    root: 3,
    shared_stable_prefix: 1,
    private_long_anchor: 1,
  })
  expect(summary.byFinishReason).toEqual({ stop_token: 5 })
  expect(summary.ttftShare.queue).toBeCloseTo(14.5 / 18.75, 12)
  expect(summary.ttftShare.prefill).toBeCloseTo(4.25 / 18.75, 12)
  // The near-zero-decode request must not drag the rate spread.
  expect(summary.decodeTokensPerSecond.max).toBeCloseTo(5000 / 3, 6)
  expect(summary.speculative.acceptRate).toBeCloseTo(100 / 150, 12)
  expect(summary.speculative.perPosition).toEqual([40, 35, 25])
})

function throughput(
  id: number,
  values: {
    energy?: ThroughputRecord['energy']
    pressure?: Partial<ThroughputRecord['context_cache']['pressure']>
    restores?: number
  },
): ThroughputRecord {
  const energy =
    values.energy === undefined
      ? null
      : {
          board_joules: 0,
          prefill_joules: 0,
          decode_joules: 0,
          idle_joules: 0,
          idle_watts: 0,
          accounted_seconds: 0,
          residual_joules: 0,
          residual_fraction: 0,
          joules_per_token: { served: null, active: null, prefill: null, decode: null },
          ...values.energy,
        }
  const pressure = {
    spill_pages: 0,
    partial_tail_cow_pages: 0,
    private_owners_degraded: 0,
    private_owners_evicted: 0,
    shared_owners_degraded: 0,
    shared_owners_evicted: 0,
    checkpoints_dropped: 0,
    searches: 0,
    search_budget_exhaustions: 0,
    maximal_fallback_selections: 0,
    historical_fork_hits: 0,
    ...values.pressure,
  }
  return {
    event: 'throughput',
    schema_version: 21,
    server_instance_id: 'serve-test-1',
    timestamp_unix_ms: 1_700_000_000_000 + id * 5000,
    artifact_type: 'ninfer_serve_request_log',
    interval_seconds: 5,
    tokens: { computed_prefill: 0, committed_decode: 0 },
    throughput_tokens_per_second: { prefill: 0, decode: 0 },
    energy,
    scheduler: {
      running: 0,
      prefilling: 0,
      decode_ready: 0,
      waiting: 0,
      materializing: 0,
      capture_pending: 0,
      terminal_pending: 0,
    },
    decode_batch: { rounds: 0, row_rounds: 0, average_size: null },
    host_work: {
      elapsed_seconds: {
        engine_boundary: 0,
        program_submit: 0,
        program_post: 0,
        engine_commit_output: 0,
        engine_maintenance: 0,
        total: 0,
      },
      device_wait_seconds: 0,
      work_class_seconds: {
        decode_host: 0,
        decode_device_wait: 0,
        prefill_host: 0,
        prefill_device_wait: 0,
        control_host: 0,
        control_device_wait: 0,
      },
      detail_subset_seconds: { admission_policy: 0, context_progress: 0, stats_publication: 0 },
      detail_invocations: { admission_policy: 0, context_progress: 0, stats_publication: 0 },
      units: { prefill: 0, control: 0 },
    },
    context_cache: {
      captures: { completed: 0, aborted: 0 },
      selections: {
        root: 0,
        private_endpoint: 0,
        private_turn_closure: 0,
        private_response_replay: 0,
        private_long_anchor: 0,
        shared_stable_prefix: 0,
        reused_prompt_tokens: 0,
      },
      last_selection: { frontier_tokens: 0 },
      state_operations: { moves: 0, forks: 0, restores: values.restores ?? 0 },
      state_transfers: {
        d2h: { count: 0, bytes: 0, seconds: 0 },
        h2d: { count: 0, bytes: 0, seconds: 0 },
        d2d: { count: 0, bytes: 0, seconds: 0 },
      },
      main_kv_transfers: {
        d2h: { pages: 0, bytes: 0, seconds: 0 },
        h2d: { pages: 0, bytes: 0, seconds: 0 },
        d2d: { pages: 0, bytes: 0, seconds: 0 },
      },
      backend_kv_transfers: {
        d2h: { pages: 0, bytes: 0, seconds: 0 },
        h2d: { pages: 0, bytes: 0, seconds: 0 },
        d2d: { pages: 0, bytes: 0, seconds: 0 },
      },
      pressure,
      occupancy: {
        device_state_slots: 0,
        host_state_slots: 0,
        device_main_kv_pages: 0,
        device_backend_kv_pages: 0,
        host_kv_bytes: 0,
        shared_active_references: 0,
      },
      actual_transfer_seconds: 0,
    },
  }
}

test('pressure summary counts only the spills that survived nowhere', () => {
  const records = [
    throughput(1, {
      pressure: { spill_pages: 100, partial_tail_cow_pages: 40, private_owners_degraded: 20 },
      restores: 7,
    }),
    throughput(2, { pressure: { shared_owners_evicted: 5, checkpoints_dropped: 2 }, restores: 3 }),
  ]
  const summary = summarizePressure(records)
  expect(summary.spillPages).toBe(100)
  // 100 - 40 COW tails - 20 owners kept alive = 40 truly lost; the second interval spilled nothing.
  expect(summary.lostPages).toBe(40)
  expect(summary.restored).toBe(10)
  expect(summary.privateOwnersDegraded).toBe(20)
  expect(summary.sharedOwnersEvicted).toBe(5)
  expect(summary.checkpointsDropped).toBe(2)
})

test('energy summary reconciles the window like the server does', () => {
  const records = [
    throughput(1, {
      energy: {
        board_joules: 1000,
        prefill_joules: 200,
        decode_joules: 400,
        idle_joules: 380,
        idle_watts: 95,
        accounted_seconds: 4,
        residual_joules: 20,
        residual_fraction: 0.02,
        joules_per_token: { served: 0.02, active: 0.018, prefill: null, decode: null },
      },
    }),
  ]
  // Token-free interval: per-token figures stay null, totals still add up.
  const summary = summarizeEnergy(records)
  expect(summary.available).toBe(true)
  expect(summary.boardJoules).toBe(1000)
  expect(summary.prefillJoules).toBe(200)
  expect(summary.decodeJoules).toBe(400)
  expect(summary.idleJoules).toBe(380)
  expect(summary.idleWatts).toBe(95)
  expect(summary.servedJoulesPerToken).toBeNull()
  expect(summary.activeJoulesPerToken).toBeNull()
  expect(summary.residualFraction).toBeCloseTo(0.02, 12)

  const withTokens = throughput(1, {
    energy: {
      board_joules: 1000,
      prefill_joules: 200,
      decode_joules: 400,
      idle_joules: 380,
      idle_watts: 95,
      accounted_seconds: 4,
      residual_joules: 20,
      residual_fraction: 0.02,
      joules_per_token: { served: 0.02, active: 0.018, prefill: 0.02, decode: 0.02 },
    },
  })
  const record = withTokens as ThroughputRecord
  record.tokens = { computed_prefill: 100, committed_decode: 200 }
  const priced = summarizeEnergy([record])
  expect(priced.servedJoulesPerToken).toBeCloseTo(1000 / 300, 12)
  expect(priced.activeJoulesPerToken).toBeCloseTo(620 / 300, 12)
  expect(priced.prefillJoulesPerToken).toBeCloseTo(2, 12)
  expect(priced.decodeJoulesPerToken).toBeCloseTo(400 / 200, 12)
})
test('record lines parse only well-formed records', () => {
  expect(parseRecordLine('')).toBeNull()
  expect(parseRecordLine('{"event": "throughput", "timestamp_unix_ms": 1}')).toBeNull()
  expect(parseRecordLine('not json')).toBeNull()
  const record = throughput(1, {})
  const line = JSON.stringify(record)
  const parsed = parseRecordLine(line)
  expect(parsed !== null && parsed.event === 'throughput').toBe(true)
})