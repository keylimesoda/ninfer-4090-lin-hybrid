// Definitions for every term the dashboard shows.
//
// One place so a reading and its explanation cannot drift, and so the wording stays specific to
// this engine rather than generic inference vocabulary. Each entry says what the number measures,
// and where it is useful, what to conclude when it moves.

export interface Definition {
  title: string
  body: string
}

export const GLOSSARY = {
  // --- Headline ------------------------------------------------------------

  decodeRate: {
    title: 'Decode rate, all lanes',
    body: 'Tokens finally committed by decode rounds per second, summed across every active lane. ' +
      'It rises with concurrency even when each request gets no faster; divide by the decode ' +
      'batch for the rate one sequence sees.',
  },
  prefillRate: {
    title: 'Prefill rate',
    body: 'Prompt suffix tokens actually computed per second, excluding prefix-reuse hits. A ' +
      'cached prompt prefills almost nothing, so a falling prefill rate with steady decode is the ' +
      'cache working, not the engine slowing down.',
  },
  lanes: {
    title: 'Lanes',
    body: 'A lane is one resident sequence the single execution thread interleaves. The ' +
      'configured maximum is --max-concurrency; running shows how many lanes are occupied right ' +
      'now.',
  },
  queued: {
    title: 'Queued requests',
    body: 'Admitted requests waiting for a lane. They occupy a pending slot; when the pending ' +
      'budget is full the server stops accepting new work, so this figure near its ceiling is ' +
      'the saturation warning.',
  },
  queueWait: {
    title: 'Queue wait',
    body: 'Wall time a request spent waiting for a lane before its prefill began, from the ' +
      'engine timing of completed requests. Long queue wait means the server is saturated, not ' +
      'slow: more lanes or shorter generations move it, not faster kernels.',
  },
  ttft: {
    title: 'Time to first token',
    body: 'From request admission to the first output token, including queue wait and prefill. ' +
    'The queue/prefill split says which of the two owned the wait.',
  },
  percentiles: {
    title: 'Percentiles',
    body: 'Nearest-rank over the retained completed requests: p50 is the middle request, p99 the ' +
      'slowest 1% boundary. The retained window is at most a few hundred requests, so p99 is a ' +
      'statement about that sample, not about the population.',
  },
  prefillAvoided: {
    title: 'Prefill avoided',
    body: 'Share of prompt tokens that prefix reuse kept out of prefill entirely. The complement ' +
      'is how much of the prompts the model still had to read. High on a chatty session that ' +
      'reuses its own history; near zero on fresh cold prompts.',
  },
  mtpAccept: {
    title: 'MTP acceptance',
    body: 'Accepted over drafted speculative tokens. With a 3-token draft window the ideal is ' +
      '100% (every proposed token matches); each rejected position falls back to a sequential ' +
      'step, which is what the per-position decay curve shows.',
  },
  energyServed: {
    title: 'Energy served',
    body: 'Board joules per token across the window, including the idle draw between requests — ' +
      'what the work actually costs. It degrades when the server is mostly idle even though ' +
      'nothing about the engine changed, which is why the active figure exists too.',
  },

  // --- Throughput panel ----------------------------------------------------

  decodeBatch: {
    title: 'Decode batch',
    body: 'Mean sequences advanced per decode round. One model traversal serves the whole ' +
      'batch, so the aggregate decode rate is this batch times the per-sequence rate.',
  },
  perSequenceRate: {
    title: 'Per-sequence rate',
    body: 'Aggregate decode rate divided by the mean batch: the rate one sequence sees. This is ' +
      'the figure a user experiences; the aggregate is the figure the hardware pays for.',
  },
  intervalBands: {
    title: 'Interval bands',
    body: 'Each sample is a rate over its own reporting interval, so it is drawn as a band whose ' +
      'width is that interval. The reporter skips intervals with no activity, so a wide band is ' +
      'idle time folded forward into the next sample, not a long sustained rate.',
  },
  snapshotSeries: {
    title: 'Snapshot series',
    body: 'Scheduler occupancy is read once per report, not averaged over the interval, so the ' +
      'chart shows the levels the engine reported, not time means. A spike that lasted ' +
      'milliseconds between reports is invisible to it.',
  },

  // --- GPU panel -----------------------------------------------------------

  gpuUtil: {
    title: 'SM utilization',
    body: 'Share of time at least one CUDA core was busy, as the driver samples it. On a ' +
      'single-threaded engine a decode round is compute-pulse-then-wait, so a healthy decoding ' +
      'server often reads well below 100% without being idle.',
  },
  gpuTemp: {
    title: 'Board temperature',
    body: 'The board sensor the driver uses for its throttle curve. On a 4090 sustained ' +
      'compute sits near the throttle point; the reading matters less than whether the throttle ' +
      'reasons light up.',
  },
  gpuPower: {
    title: 'Board power',
    body: 'Instantaneous board draw against the active power limit. Pinning the limit while the ' +
      'draw holds at the ceiling means the work is power-limited; every token after that point ' +
      'costs its full watt-second.',
  },
  gpuClock: {
    title: 'SM clock',
    body: 'Current against maximum shader clock. A clock well below its ceiling while utilization ' +
      'is high means the board is being held back by a throttle reason, not by the workload.',
  },
  gpuMemBw: {
    title: 'Memory bandwidth utilization',
    body: 'Share of peak memory bandwidth in use. Weight-heavy decode on this target is ' +
      'bandwidth-bound, so this, not SM utilization, is the ceiling a decode round runs into.',
  },
  gpuThrottle: {
    title: 'Active throttle reasons',
    body: 'Driver-reported reasons the board is not running at its maximum clocks. gpu_idle and ' +
      'a user-set clock cap are filtered out because they are not limits on work the engine ' +
      'wanted to do.',
  },
  boardMemory: {
    title: 'Board memory',
    body: 'Everything resident on the board: every arena, the KV cache, and whatever else the ' +
      'process touched. The VRAM panel decomposes it; this meter shows the remainder after the ' +
      'decomposition.',
  },

  // --- VRAM panel ----------------------------------------------------------

  vramBudget: {
    title: 'VRAM budget',
    body: 'The board memory left after every arena was reserved at startup. It is the headroom ' +
      'the engine can still hand out; when a new sequence does not fit, that is where the ' +
      'rejection comes from.',
  },
  weightsArena: {
    title: 'Weights arena',
    body: 'The dequantized model weights, resident for the whole process. Its capacity is fixed ' +
      'at startup and is the largest single block on a 24 GB board.',
  },
  kvArena: {
    title: 'Sequence arena',
    body: 'The KV cache and per-sequence state, sized from --kv-capacity. It holds the prompt ' +
      'and generation state of every resident lane plus the context cache the server keeps for ' +
      'reuse.',
  },
  workspaceArena: {
    title: 'Workspace arena',
    body: 'Scratch for the ops that run per step. Its peak is the high-water mark over the ' +
      'process life; a peak near capacity means the next larger batch or prompt shape will ' +
      'fail to allocate, so it is read as a bound, not a live gauge.',
  },
  cudaGraphs: {
    title: 'CUDA graphs',
    body: 'Memory the captured graphs hold against their allowance. A graph replay is the fast ' +
      'path for decode; the allowance is the budget that capture was limited to at startup.',
  },
  kvPayload: {
    title: 'KV payload',
    body: 'Bytes of actual KV data held in the sequence arena, broken down by what it stores. ' +
      'Compressed modes store rotated keys with 4-bit values, which is why the payload is far ' +
      'smaller than the token count times a head size would suggest.',
  },
  pageGroups: {
    title: 'Page groups',
    body: 'KV capacity in groups of contiguous pages. The engine allocates in groups so a ' +
      'sequence can grow without fragmenting the arena; the maximum is what the budget can ' +
      'support, the current value what it holds.',
  },
  textKv: {
    title: 'Text KV',
    body: 'Keys and values of the main text attention, the payload every prompt and token adds.',
  },
  gdnState: {
    title: 'GDN state',
    body: 'The recurrent state of the gated-delta-net layers, one small block per sequence ' +
      'instead of a growing history. It is what makes a long context cheap in memory.',
  },
  mtpKv: {
    title: 'MTP KV',
    body: 'KV state of the multi-token-prediction draft head, held so a draft step does not ' +
      'recompute what the main path already computed.',
  },
  replayRecords: {
    title: 'Replay records',
    body: 'Device-side records retained so a sequence can be replayed from a checkpoint without ' +
      're-reading its full history.',
  },
  headroom: {
    title: 'KV headroom',
    body: 'Capacity left inside the KV budget before the next page group must be allocated. ' +
      'Exhaustion is what turns a new admission into a queue wait or a rejection.',
  },
  hostKv: {
    title: 'Host KV',
    body: 'The RAM-side copy of evicted KV and the host state slots the context cache spills ' +
      'into. A prompt that lives on host is reusable, but reusing it pays a device import.',
  },

  // --- Scheduler panel -----------------------------------------------------

  laneOccupancy: {
    title: 'Lane occupancy',
    body: 'Running lanes against the configured maximum. Full does not by itself mean slow: ' +
      'the decode batch is what turns occupancy into aggregate rate.',
  },
  ingressQueue: {
    title: 'Ingress queue',
    body: 'Waiting requests against the pending budget. The queue is bounded on purpose; ' +
      'filling it is the server saying it cannot admit more work, which protects the lanes ' +
      'already running.',
  },
  workerSplit: {
    title: 'Execution thread split',
    body: 'One thread executes everything, so its wall time partitions into the phases the ' +
      'engine names. A second spent in any phase is a second no resident lane advances; the ' +
      'device-wait portion is the thread parked while the GPU works and is not contention.',
  },
  workerBoundary: {
    title: 'Engine boundary',
    body: 'Thread time at the Engine API edges: submitting work and collecting results, the ' +
      'handshakes that are not model execution.',
  },
  workerSubmit: {
    title: 'Program submit',
    body: 'Time building and handing a work program to the device, including the graph-launch ' +
      'path.',
  },
  workerPost: {
    title: 'Program post',
    body: 'Post-execution work after the device finishes: result collection and output ' +
      'committing on the host.',
  },
  workerCommit: {
    title: 'Output commit',
    body: 'Committing produced output into the sequence and the consumer queues: where tokens ' +
      'become readable.',
  },
  workerMaintenance: {
    title: 'Maintenance',
    body: 'Thread time on upkeep that is not a phase of the work itself: statistics, failure ' +
      'cleanup, bookkeeping.',
  },
  workerDeviceWait: {
    title: 'Device wait',
    body: 'The execution thread parked while the GPU runs. It is not contention for the thread; ' +
      'it is the complement of the thread split, and a healthy server spends most of its wall ' +
      'time here during compute pulses.',
  },
  workerDetail: {
    title: 'Slow-path detail',
    body: 'Invocation counts and time for the paths that are expensive when they fire: ' +
      'admission planning, context-cache progress, and stats publication. They are subsets of ' +
      'the top-level phases, never extra time on top of them.',
  },
  decodeRounds: {
    title: 'Decode rounds',
    body: 'Model traversals that advanced the decode batch. Row rounds count lanes, so rows ' +
      'over rounds is the batch. Abandoned rounds are traversals a fault discarded partway ' +
      'through: the engine retired the round and kept serving.',
  },

  // --- Slots panel ---------------------------------------------------------

  laneRetained: {
    title: 'Retained lane',
    body: 'A lane that finished but keeps its resident session in VRAM, so a matching prompt ' +
      'can reuse it without any import. Retention is what makes a follow-up turn fast.',
  },
  slotReused: {
    title: 'Reuse depth',
    body: 'Cached over prompt tokens for the lane: the share of the prompt the resident prefix ' +
      'already covered when the lane was last used.',
  },
  sessionDigest: {
    title: 'Session digest',
    body: 'A short identity for the resident session, so two lanes holding the same session are ' +
      'recognizable at a glance.',
  },
  checkpoints: {
    title: 'Checkpoints',
    body: 'Rewrite checkpoints the session keeps in the lane: divergence points an earlier ' +
      'prefix can jump back to without recomputing.',
  },

  // --- Cache panel ---------------------------------------------------------

  contextCache: {
    title: 'Context cache',
    body: 'The resident store of prompt state the engine reuses across requests: device KV ' +
      'pages and state slots on top, host RAM underneath. There is no tier ladder; state is ' +
      'either resident on device or it was spilled, and a reuse of spilled state pays a host ' +
      'import.',
  },
  kvPages: {
    title: 'Device KV pages',
    body: 'KV pages resident on device against the page-group budget. The main and backend ' +
      'pools are shown separately because they hold different data and spill under different ' +
      'pressure.',
  },
  stateSlots: {
    title: 'Device state slots',
    body: 'Resident slots for per-sequence state against the configured capacity, which ' +
      'includes the lanes themselves. A slot held by the cache is a slot a new request cannot ' +
      'use until it is spilled or dropped.',
  },
  pressureActions: {
    title: 'Pressure actions',
    body: 'What the cache did to stay inside its budget: pages spilled to host, owners degraded ' +
      'or evicted, checkpoints dropped. Eviction is the cache working, not failing; the ' +
      'distinction that matters is between state that moved and state that was lost.',
  },
  ownerDegradation: {
    title: 'Owner degradation',
    body: 'A request or session whose resident state no longer fit and was demoted to a ' +
      'shallower form rather than dropped. Its next reuse is slower but not a recompute.',
  },
  lostPages: {
    title: 'Lost pages',
    body: 'Pages that spilled whole, were not a COW tail, and no owner was demoted to keep ' +
      'them alive: they are gone, so the next use recomputes them. This is the counter that ' +
      'turns cache pressure into wasted work.',
  },
  transfer: {
    title: 'State transfers',
    body: 'Device-host and device-device movement of state and KV: d2h is a spill, h2d an ' +
      'import. The import bytes are what a reuse of non-resident state costs; the seconds are ' +
      'what it costs in wall time.',
  },
  reusePath: {
    title: 'Reuse path',
    body: 'How a request got its prefix without computing it: the selection that matched, from ' +
      'a private endpoint or turn closure to a shared stable prefix. A request on the root path ' +
      'reused nothing and prefilled its whole prompt.',
  },
  reuseBreakdown: {
    title: 'Reuse breakdown',
    body: 'Completed requests grouped by the selection that served their prefix. The mix ' +
      'shifts as sessions age: fresh traffic is root, a chatty session is its own closures, ' +
      'and a shared system prompt is the stable prefix.',
  },

  // --- Requests panel ------------------------------------------------------

  promptSource: {
    title: 'Reuse path',
    body: 'The selection path that served the request\u2019s prefix. Coloured by path; ' +
      'root means the whole prompt was computed.',
  },
  ttftSplit: {
    title: 'TTFT split',
    body: 'Each request\u2019s time to first token divided into queue wait and prefill, the ' +
      'two terms the engine accounts for. A queue-dominant TTFT is a capacity problem; a ' +
      'prefill-dominant one is a prompt-size or cache problem.',
  },
  finishReason: {
    title: 'Finish reason',
    body: 'How the generation ended: a stop condition, an output budget, context capacity, or ' +
      'cancellation. Anything but a stop condition is worth a second look in a long window.',
  },

  // --- Modes ---------------------------------------------------------------

  replayMode: {
    title: 'Replay mode',
    body: 'The dashboard is reading a JSONL file instead of a live server. Records and the ' +
      'server_start replay fine; board sensors, lane occupancy, and anything only the running ' +
      'engine knows read as absent.',
  },
  energyActive: {
    title: 'Energy active',
    body: 'Board joules per token with the measured idle baseline removed: the cost that ' +
      'tracks the schedule rather than the duty cycle. On a busy server it approaches the ' +
      'served figure; on a mostly idle one it stays flat while served climbs.',
  },
  energyPrefill: {
    title: 'Energy prefill',
    body: 'Prefill joules per computed prefill token, measured around each prefill execution ' +
      'unit. It prices the prompt-reading phase alone; tokens a prefix hit reused are not in ' +
      'the denominator, so a good cache does not flatter this figure by hiding work.',
  },
  energyDecode: {
    title: 'Energy decode',
    body: 'Decode joules per committed token. Decode draws far more energy per token than ' +
      'prefill on this target, so this is usually the larger of the two phase figures even ' +
      'though prefill does more compute per step.',
  },
  energyIdle: {
    title: 'Idle draw',
    body: 'Board power measured over intervals that ran nothing and had nothing queued — the ' +
      'baseline the server pays even between requests. The interval\u2019s unclaimed energy is ' +
      'priced at this baseline, which is why it is measured rather than assumed.',
  },
  energyPerMillion: {
    title: 'Energy per million tokens',
    body: 'The per-token figure restated in watt-hours over a million tokens, the denominator ' +
      'inference is priced in. Multiply by a local rate for a $/1M figure comparable to ' +
      'published prices; it is the same measurement, not a new one.',
  },
  energyResidual: {
    title: 'Energy residual',
    body: 'Share of measured board energy the phase split and the idle baseline do not ' +
      'explain. The split integrates instantaneous power while the total comes from the ' +
      'board\u2019s own counter, so the residual is the estimator\u2019s error, published ' +
      'rather than hidden. The total is exact regardless of it.',
  },
} as const satisfies Record<string, Definition>

export type GlossaryKey = keyof typeof GLOSSARY