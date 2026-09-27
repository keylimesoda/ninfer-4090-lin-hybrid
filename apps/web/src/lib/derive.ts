// Derived request and cache analytics over the retained records.

import { CHART } from './palette'
import type { RequestDoneRecord, ThroughputRecord } from './records'

/**
 * Nearest-rank percentile with truncating index selection, not an interpolating definition: the
 * two disagree on small samples, and one rule everywhere keeps the dashboard's numbers
 * reproducible by hand from the raw log.
 */
export function percentile(values: readonly number[], quantile: number): number {
  if (values.length === 0) return 0
  const sorted = [...values].sort((a, b) => a - b)
  const index = Math.min(sorted.length - 1, Math.trunc(quantile * sorted.length))
  return sorted[index]!
}

export function mean(values: readonly number[]): number {
  if (values.length === 0) return 0
  return values.reduce((total, value) => total + value, 0) / values.length
}

export function sum(values: readonly number[]): number {
  return values.reduce((total, value) => total + value, 0)
}

function tally<T extends string>(values: readonly T[]): Record<string, number> {
  const counts: Record<string, number> = {}
  for (const value of values) counts[value] = (counts[value] ?? 0) + 1
  return counts
}

export interface Spread {
  p50: number
  p90: number
  p99: number
  max: number
  mean: number
}

function spread(values: readonly number[]): Spread {
  return {
    p50: percentile(values, 0.5),
    p90: percentile(values, 0.9),
    p99: percentile(values, 0.99),
    max: values.length === 0 ? 0 : Math.max(...values),
    mean: mean(values),
  }
}

export interface SpeculativeSummary {
  drafted: number
  accepted: number
  /** Accepted fraction of drafted tokens, the headline MTP figure. */
  acceptRate: number
  /** Acceptance count per draft position, summed across requests. */
  perPosition: number[]
  fallbackSteps: number
}

export interface RequestSummary {
  count: number
  promptTokens: Spread
  computedPrefill: Spread
  generated: Spread
  totalPromptTokens: number
  totalComputedPrefill: number
  /** Fraction of prompt tokens that prefix reuse kept out of prefill entirely. */
  prefillAvoided: number
  overLongGenerations: number
  byReusePath: Record<string, number>
  byFinishReason: Record<string, number>
  ttft: Spread
  /** Queue wait before prefill, separately from the TTFT phases the engine reports. */
  queueWait: Spread
  /** Share of summed TTFT attributable to each phase. These are the actionable latency terms. */
  ttftShare: { queue: number; prefill: number }
  speculative: SpeculativeSummary
  decodeTokensPerSecond: Spread
}

export function summarizeRequests(records: readonly RequestDoneRecord[]): RequestSummary {
  const prompt = records.map((record) => record.result.prompt_tokens)
  const computed = records.map((record) => record.result.computed_prefill_tokens)
  const generated = records.map((record) => record.result.completion_tokens)
  const ttft = records.map((record) => record.timings_seconds.ttft)
  // The engine reports the queue wait separately from the TTFT phases, and it is the term an
  // operator can act on: a long queue means the server is saturated, not slow.
  const queue = records.map((record) => record.engine_timing.queue_wait_seconds)

  const totalPrompt = sum(prompt)
  const totalComputed = sum(computed)
  const totalTtft = sum(ttft)

  const perPosition: number[] = []
  let drafted = 0
  let accepted = 0
  let fallbackSteps = 0
  for (const record of records) {
    const speculative = record.speculative
    if (!speculative) continue
    drafted += speculative.drafted_tokens
    accepted += speculative.accepted_tokens
    fallbackSteps += speculative.fallback_steps
    speculative.accepted_per_position.forEach((value, index) => {
      perPosition[index] = (perPosition[index] ?? 0) + value
    })
  }

  // Only requests that actually decoded contribute a rate; a fully cached prompt that emitted
  // one token in near-zero time would otherwise dominate the spread with a meaningless value.
  const decodeRates = records
    .filter((record) => record.timings_seconds.decode > 0.01)
    .map((record) => record.result.completion_tokens / record.timings_seconds.decode)

  return {
    count: records.length,
    promptTokens: spread(prompt),
    computedPrefill: spread(computed),
    generated: spread(generated),
    totalPromptTokens: totalPrompt,
    totalComputedPrefill: totalComputed,
    prefillAvoided: totalPrompt === 0 ? 0 : 1 - totalComputed / totalPrompt,
    overLongGenerations: generated.filter((value) => value > 4096).length,
    byReusePath: tally(records.map((record) => record.result.prefix_reuse_path)),
    byFinishReason: tally(records.map((record) => record.result.finish_reason)),
    ttft: spread(ttft),
    queueWait: spread(queue),
    ttftShare: {
      queue: totalTtft === 0 ? 0 : sum(queue) / totalTtft,
      prefill: totalTtft === 0 ? 0 : sum(records.map((r) => r.timings_seconds.prefill)) / totalTtft,
    },
    speculative: {
      drafted,
      accepted,
      acceptRate: drafted === 0 ? 0 : accepted / drafted,
      perPosition,
      fallbackSteps,
    },
    decodeTokensPerSecond: spread(decodeRates),
  }
}

/**
 * Context-cache pressure over the retained throughput window.
 *
 * Eviction on its own is the cache doing its job, so none of these are pathological in isolation.
 * The readings that matter are `lost` — state discarded with no host handoff, guaranteed to be
 * recomputed if wanted again — and the transfer mix: a working set that no longer fits resident
 * keeps its hit rate and quietly starts paying a host import on every turn.
 *
 * Summed from interval deltas rather than differencing endpoints, so a server restart inside the
 * window cannot turn a counter reset into a negative or absurd reading.
 */
export interface PressureSummary {
  spillPages: number
  partialTailCowPages: number
  privateOwnersDegraded: number
  privateOwnersEvicted: number
  sharedOwnersDegraded: number
  sharedOwnersEvicted: number
  checkpointsDropped: number
  searches: number
  searchBudgetExhaustions: number
  maximalFallbackSelections: number
  /** Spilled with no COW tail and no owner demotion to keep it live: gone, not moved. */
  lostPages: number
  restored: number
  /** Host-bound state movement: the bytes a reuse had to import instead of reading VRAM. */
  importedBytes: number
  importedSeconds: number
}

export function summarizePressure(records: readonly ThroughputRecord[]): PressureSummary {
  let spillPages = 0
  let partialTailCowPages = 0
  let privateOwnersDegraded = 0
  let privateOwnersEvicted = 0
  let sharedOwnersDegraded = 0
  let sharedOwnersEvicted = 0
  let checkpointsDropped = 0
  let searches = 0
  let searchBudgetExhaustions = 0
  let maximalFallbackSelections = 0
  let restored = 0
  let importedBytes = 0
  let importedSeconds = 0
  let lostPages = 0
  for (const record of records) {
    const pressure = record.context_cache.pressure
    spillPages += pressure.spill_pages
    partialTailCowPages += pressure.partial_tail_cow_pages
    privateOwnersDegraded += pressure.private_owners_degraded
    privateOwnersEvicted += pressure.private_owners_evicted
    sharedOwnersDegraded += pressure.shared_owners_degraded
    sharedOwnersEvicted += pressure.shared_owners_evicted
    checkpointsDropped += pressure.checkpoints_dropped
    searches += pressure.searches
    searchBudgetExhaustions += pressure.search_budget_exhaustions
    maximalFallbackSelections += pressure.maximal_fallback_selections
    restored += record.context_cache.state_operations.restores
    // A page that spilled whole (not a COW tail) and no owner was demoted to keep it resident
    // did not survive anywhere; the next use recomputes it.
    if (pressure.spill_pages > pressure.partial_tail_cow_pages) {
      const pureSpills = pressure.spill_pages - pressure.partial_tail_cow_pages
      const keptAlive = pressure.private_owners_degraded + pressure.shared_owners_degraded
      if (pureSpills > keptAlive) lostPages += pureSpills - keptAlive
    }
    const d2h = record.context_cache.state_transfers.d2h
    importedBytes += d2h.bytes
    importedSeconds += d2h.seconds
  }
  return {
    spillPages,
    partialTailCowPages,
    privateOwnersDegraded,
    privateOwnersEvicted,
    sharedOwnersDegraded,
    sharedOwnersEvicted,
    checkpointsDropped,
    searches,
    searchBudgetExhaustions,
    maximalFallbackSelections,
    lostPages,
    restored,
    importedBytes,
    importedSeconds,
  }
}

export interface EnergySummary {
  /** False when no record in the window carried energy, i.e. the board has no counter. */
  available: boolean
  boardJoules: number
  prefillJoules: number
  decodeJoules: number
  idleJoules: number
  /** Latest measured idle draw, in watts. */
  idleWatts: number
  prefillTokens: number
  decodeTokens: number
  /**
   * Joules per token, or null where no tokens of that kind were produced.
   *
   * `served` prices every joule the board drew across the window, including the idle draw between
   * requests; it is what the work actually costs and it degrades when the server is mostly idle.
   * `active` removes the measured idle baseline and tracks the schedule rather than the duty
   * cycle. They are both reported because neither answers the other's question.
   */
  servedJoulesPerToken: number | null
  activeJoulesPerToken: number | null
  prefillJoulesPerToken: number | null
  decodeJoulesPerToken: number | null
  /**
   * Share of measured energy the phase splits and idle baseline together do not explain.
   *
   * The board refreshes power at roughly 50 Hz while a decode round is shorter than that, so the
   * split is an estimate over a measured total. A large residual means the split should not be
   * read closely; the total remains exact either way.
   */
  residualFraction: number
}

/** Energy per token, or null when the denominator is zero: no tokens is not zero joules each. */
function perToken(joules: number, tokens: number): number | null {
  return tokens === 0 ? null : joules / tokens
}

export function summarizeEnergy(records: readonly ThroughputRecord[]): EnergySummary {
  let boardJoules = 0
  let prefillJoules = 0
  let decodeJoules = 0
  let idleJoules = 0
  let residualJoules = 0
  let prefillTokens = 0
  let decodeTokens = 0
  let idleWatts = 0
  let available = false
  for (const record of records) {
    const energy = record.energy
    if (!energy) continue
    available = true
    boardJoules += energy.board_joules
    prefillJoules += energy.prefill_joules
    decodeJoules += energy.decode_joules
    idleJoules += energy.idle_joules
    residualJoules += energy.residual_joules
    prefillTokens += record.tokens.computed_prefill
    decodeTokens += record.tokens.committed_decode
    // The baseline is a running calibration, so the most recent reading is the current one.
    idleWatts = energy.idle_watts
  }
  const tokens = prefillTokens + decodeTokens
  return {
    available,
    boardJoules,
    prefillJoules,
    decodeJoules,
    idleJoules,
    idleWatts,
    prefillTokens,
    decodeTokens,
    servedJoulesPerToken: perToken(boardJoules, tokens),
    activeJoulesPerToken: perToken(Math.max(0, boardJoules - idleJoules), tokens),
    prefillJoulesPerToken: perToken(prefillJoules, prefillTokens),
    decodeJoulesPerToken: perToken(decodeJoules, decodeTokens),
    residualFraction: boardJoules === 0 ? 0 : residualJoules / boardJoules,
  }
}

/**
 * Reuse-path colors, shared by the reuse breakdown and the cache panel.
 *
 * Literal values rather than `var()` because the same palette feeds ECharts, which renders to
 * canvas and cannot resolve custom properties.
 */
export const REUSE_PATH_COLOR: Record<string, string> = {
  root: CHART.dim,
  private_endpoint: CHART.accent,
  private_turn_closure: CHART.blue,
  private_response_replay: CHART.violet,
  private_long_anchor: CHART.warning,
  shared_stable_prefix: CHART.muted,
}