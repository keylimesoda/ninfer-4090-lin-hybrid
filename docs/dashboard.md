# Dashboard

An optional single-page dashboard for one running `ninfer-serve`. It answers, at a glance, what
the engine is doing: current prefill and decode rates, whether requests are queueing for a lane,
where prompts are being served from, how the VRAM budget is spent, and whether the board is the
limit. It also loads a `--request-log-jsonl` file to analyze a past session offline.

The application lives in [`apps/web/`](../apps/web/) and is built with Bun, Vite, and React.

## Build and run

The Docker image builds the dashboard in its own stage and serves it from `/opt/ninfer/web`, so
nothing below is needed to use it there — `http://127.0.0.1:8080/` is the dashboard and
`http://127.0.0.1:8080/v1` is the API, on one port.

To build it outside Docker:

```bash
cd apps/web
bun install
bun run build
```

Then serve it from the engine itself, same-origin with the API:

```bash
./build-sm89/apps/ninfer-serve models/qwen3_8_27b.ninfer --web-dir apps/web/dist
```

Open `http://127.0.0.1:8080/`. Registered API routes are matched before the static mount, so the
dashboard cannot shadow an endpoint; any other path resolves to the application shell.

For development against a running engine:

```bash
cd apps/web
NINFER_BASE_URL=http://127.0.0.1:8080 bun run dev
```

The dev server proxies `/telemetry`, `/events`, `/metrics`, `/slots`, `/health`, and `/v1` to that
origin, so `--cors` is not required. `NINFER_BASE_URL` is validated as an origin and defaults to
`http://127.0.0.1:8080`.

| Command | Purpose |
|---|---|
| `bun run dev` | development server on `127.0.0.1:5180` |
| `bun run build` | typecheck and emit `dist/` |
| `bun run typecheck` | types only |
| `bun test` | derivation-layer tests |
| `bun run format` | Prettier write over the app sources |
| `bun run format:check` | Prettier check |

## Data sources

The dashboard reads two channels because they answer different questions.

| Channel | Kind | Carries |
|---|---|---|
| `GET /telemetry` | polled at 1 Hz | levels: board sensors, scheduler occupancy, VRAM, context-cache fill |
| `GET /events` | SSE | history: throughput samples, board energy, and completed requests |

Levels cannot be reconstructed by replaying deltas, and the event stream is bounded and lossy
under backpressure, so the poll is always authoritative for current state. `/metrics` is not read
by the dashboard: every counter it needs is already in the two channels above, in a form that does
not require differencing scrapes.

Board energy is the one sensor reading that is carried in the record stream rather than only in
the poll, because it is an interval total rather than a level. The Energy panel therefore works on
a replayed log; the GPU panel, which reports levels, does not.

## Energy

The Energy panel reports what the work costs, in joules. A watt-second is a joule, so tokens per
watt-second and tokens per joule are the same figure; the panel reports joules per token, because
energy composes additively across phases while a rate does not.

Two denominators are shown because they answer different questions and neither substitutes for the
other. **Served** divides all board energy by all tokens, including the draw while idle between
requests; it is what the work actually costs and it degrades on a mostly idle server even when
nothing about the engine changed. **Active** removes the measured idle baseline and tracks the
schedule rather than the duty cycle, which is the figure to compare between two builds. On a board
that idles near 20 W the two differ by a large factor, so showing only one would be misleading.

Prefill and decode are split apart because prefill is compute-bound and draws far more power than
memory-bound decode; attributing energy in proportion to time would systematically understate
prefill. Prefill divides by computed prefill tokens, so prefix-reuse hits do not flatter the
kernels, and decode divides by committed decode tokens, which under MTP counts accepted tokens
only — speculation burns compute on drafts that may be rejected, so it can raise tokens per second
and joules per token at the same time.

The board total is exact; the split is not. It is integrated from power samples the board refreshes
at roughly 50 Hz, which is coarser than a decode round, so the panel publishes the residual — the
share of measured energy the split and idle baseline together fail to explain — and marks the phase
figures once it exceeds a tenth of the total. A few percent is the expected steady state and
shrinks with window length. Boards with no cumulative energy counter report the panel as
unavailable rather than as zero.

The **per 1M tokens** stat restates the served figure in watt-hours, which is the denominator
inference is priced in and the unit electricity is billed in; one multiplication by a local rate
makes it comparable to a published $/1M-token price. It is an exact rescale of joules per token by
`1e6/3600` and carries no extra information. The per-token figures stay primary because energy
composes additively across phases and a rescaled rate does not — prefill and decode joules-per-token
can be combined against their own token counts, which is the operation the panel is built around.

## Reading the charts

Two sampling semantics are deliberately drawn differently, because conflating them would assert
measurements that were never taken.

- **Throughput** is an average over each reporting interval, so a sample fills its whole interval
  as a band. The reporter skips intervals with no activity and folds the skipped time into the
  next sample's `interval_seconds`, so a wide band is idle time folded forward, not a long
  sustained rate.
- **Scheduler occupancy** (`running`, `waiting`) is a snapshot taken when the report was emitted,
  not an interval average, so it is drawn as points at their own timestamps.

Decode throughput is engine-wide: the engine sums committed tokens over every lane in a round,
so the figure rises with concurrency even when no individual request gets faster. The panel pairs
it with the mean decode batch and the per-sequence rate (aggregate divided by batch), which is what
a single client experiences. Both decode means are taken over intervals that ran a decode round
rather than the whole window, so idle time does not deflate them. Per-request rates in the Requests
table are not a partition of the aggregate — every lane in a round is charged the full round wall
time — so they do not sum to it.

There is no per-lane throughput breakdown, because the engine keeps no per-lane token or round
counter; the slot telemetry publishes occupancy only.

Prefill and decode are plotted on separate rate scales over one shared time axis. On this target
prefill runs roughly an order of magnitude faster than decode, so a single linear axis renders
decode as a flat sliver; each series is therefore read against its own axis, and the interval
tooltip reports both rates so they remain directly comparable at any instant.

Every reading carries an explanatory tooltip. Panel headings, stat captions, legend entries,
stacked-bar segments, and table headers are hover and focus targets that define the term, name the
units, and say what it means when the value moves. The wording lives in one glossary module rather
than beside each panel, so a reading and its explanation cannot drift apart.

The execution-thread wall-clock split is the panel that explains contention: one mutex serializes
the phases the engine names — boundary, submit, post, output commit, and upkeep — so a second
spent in any of them is a second in which no other resident lane advances. Device wait, the thread
parked while the GPU runs, is reported beside the split rather than inside it, and the slow-path
detail (admission planning, context-cache progress, stats publication) breaks the phases open for
the paths that are expensive when they fire.

TTFT is decomposed into queue wait and prefill. A queue-dominated TTFT means requests are waiting
for a lane rather than being computed, which more lanes or shorter generations address before any
kernel work does; a prefill-dominated TTFT is prompts being recomputed, and the reuse breakdown
below says from where.

## Context cache

The cache panel answers what the context cache is holding and what it did to keep holding it.
There is no tier ladder: device KV pages and per-sequence state slots sit on top, host RAM
underneath, and the engine moves state between the two under pressure.

Eviction on its own is a cache working normally, so no single count here is pathological. The
readings that carry meaning are:

- **lost pages** — pages that spilled whole, were not a copy-on-write tail, and no owner was
  demoted to keep them alive: they are gone, so the next use recomputes them. This is the
  counter that says the cache stopped paying its way, and it is estimated as spill pages minus
  the copy-on-write tails and owner degradations that kept state live.
- **owner degradation** — a request or session whose resident state no longer fit and was demoted
  to a shallower form rather than dropped. Its next reuse is slower but not a recompute; a
  degradation is recoverable, an eviction is not.
- **checkpoint drops** — rewrite checkpoints discarded under pressure. A later turn that diverges
  from a dropped point recomputes from further back.
- **search budget exhaustion and maximal fallbacks** — the cache's own planning running out of
  budget, the sign that the working set is too large to manage cheaply.

The reuse breakdown groups completed requests by the selection that served their prefix, from the
root path (the whole prompt was computed) through private endpoints, turn closures, response
replays, and long anchors to a shared stable prefix. The mix shifts as sessions age: fresh traffic
is root, a chatty session is its own closures. A working set that has outgrown the device keeps
serving hits and simply starts paying a device import on every reuse; the transfer rows report the
bytes and seconds of exactly that.

The reuse-path and pressure counters in the panel come from the interval deltas the throughput
record already carries, so the panel works on a replayed log; the device KV pages and state slots
it is measured against are the same fields the live poll reports, read from the log's own
`server_start` configuration.

## Replay

`load jsonl` reads a `--request-log-jsonl` file and renders it through the same components. When a
file contains records from more than one server instance — it is opened in append mode — only the
last instance is kept, because mixing two configurations on one axis would misattribute every
derived figure.

A log carries request and throughput history plus the `server_start` configuration, so throughput,
latency, cache occupancy against configured capacity, energy, and per-request analysis are all
available offline. Board telemetry and live slot occupancy are sampled, never recorded, and those
panels say so rather than showing a stale or zero reading.

## Derived analytics

The request aggregations — percentiles, reuse-path distribution, finish-reason partitioning, TTFT
decomposition, and queue-wait percentiles — follow definitions pinned by fixture tests in
`src/lib/derive.test.ts`, including the truncating nearest-rank percentile rule, so the dashboard
and the engine's own counters cannot report different numbers for the same log.

Pressure and transfer totals are summed from the interval deltas the throughput record already
carries, rather than by differencing the cumulative endpoints of the window, so a server restart
inside the window cannot turn a counter reset into a negative or absurd reading. A counter that a
replayed log predates reads as zero rather than as `NaN` in a total.