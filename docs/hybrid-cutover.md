# Hybrid port report and cutover

Status: complete. The branch `ninfer-4090-lin-hybrid` carries the tensorninja observability
build as five stacked commits on upstream `aeeba414`, pushed to
`keylimesoda/ninfer-4090-lin-hybrid`:

| Commit | Layer |
|---|---|
| `bb342380` | INT8 tensor-core prefill routes for dense-body GEMMs (fork A8 work, onto upstream ops) |
| `234cfbbc` | `/telemetry` snapshot + `/events` SSE stream with NVML board telemetry |
| `68c8a1f3` | faulted decode round is abandoned instead of retiring the engine |
| `fb61c8fa` | board energy from the GPU's own counter, attributed to prefill/decode across metrics, telemetry, request log, and CLI |
| `8bacecb2` | prefill progress on the Responses stream (`prompt_progress` in `response.in_progress`) |
| `d7acef5d` | `--web-dir` static serving + the `apps/web` dashboard + Dockerfile web stage + docs |

## What the port contains

- **Telemetry core**: `EventStream` owns the request-log schema instance (process identity,
  record timestamps) and fans each formatted record to the optional `--request-log-jsonl` file
  and every `GET /events` subscriber; live and file readers see byte-identical lines. A
  late subscriber is replayed the retained `server_start` plus a bounded ring.
- **`GET /telemetry`**: one complete snapshot — board sensors with decoded throttle reasons,
  scheduler occupancy with the execution-thread wall-clock split, context-cache state against
  its configured capacities, per-slot occupancy, the full `MemorySummary`.
- **Board energy**: `core::PowerMeter` over NVML owns the cumulative counter; the interval
  reporter attributes joules to prefill/decode/idle and publishes the residual. Boards without
  the counter degrade to absent readings, never zeros.
- **Round fault recovery**: a decode round discarded by a device fault abandons that round and
  keeps the engine serving, instead of taking the process down.
- **Responses prefill progress**: `response.in_progress` carries a `prompt_progress`
  object (`total/cache/processed/time_ms`); monotonicity violations throw like the Chat stream.
- **Dashboard** (`apps/web`, schema-21 frontend): headline, throughput, scheduler (occupancy,
  abandoned rounds, thread split), GPU, energy, VRAM arenas, context cache (device/host
  occupancy, pressure actions, transfers, reuse-path breakdown), latency (queue/prefill TTFT
  split), slots, and per-request detail, live or replayed from a JSONL log. Served by
  `--web-dir` from the server's own root; the Dockerfile builds it in a Bun stage.

The fork's engine-side schema is not what ships here: records and telemetry are the upstream
shapes (schema-21 records, upstream scheduler and context-cache fields), and the frontend was
rebuilt to match. The fork's Adapters and Churn panels have no upstream surface and are not
ported.

## Verification

- Full CMake build of every target in the CUDA 13.1 container: 247/247, clean.
- CPU-safe serve tests with the NVML stub shim: `serve_options`, `request_log`,
  `serve_metrics`, `row_commit`, `openai_responses` (+ store), `anthropic_schema`,
  `openai_schema`, `http_error_handler`, `http_transport`, `cli_options`, `slot_files`,
  `slot_spill_guard`, `tool_call_parser`, `pretty_logging` — all pass. GPU-device tests
  (`state_store`, `candidate_selector`, the `*_real_test` suite) need a real device.
- Dashboard: `bun test` 5/5 and the production build (`tsc -b` + Vite) emit `dist/`.
- **Not yet verified**: a live engine on the 4090 — every panel against real telemetry, the
  SSE stream under load, and board energy on the real counter. The production GPU is fully
  allocated to the current container, so this happens in the cutover window below.

## What was not ported, and why

- LoRA/adapter surface: upstream has no adapter bank to observe.
- The L1/L2/L3 continuation cache and its flags: upstream has one device/host context cache
  instead. Every `--continuation-cache*` flag and `--prefix-checkpoint-policy` is absent from
  upstream `serve_options` and must not appear on the command line.
- `scripts/prefill_probe.py`: fork-only tooling.
- Fork's `docs/performance.md` energy sections: fork-specific report genre; upstream's
  performance docs are unchanged.

## Current production profile

The validated RTX 4090 default is text-only MTP3 with CUDA Graph decode and an explicit
262144-token KV capacity. Vision is opt-in because enabling it prevents this full capacity from
admitting. DFlash2 K7 remains available for bounded structured-output workloads, but it reduced
acceptance and queue performance on long, tool-heavy agent histories.

## Cutover procedure

The hybrid image's `CMD` is the MTP3 production profile above. It listens on
`127.0.0.1:8080`, uses `--max-concurrency 1` and `--pending-timeout-ms 600000`, and includes
the request log and dashboard. The named `ninfer-continuations` volume remains mounted at
`/var/cache/ninfer`.

One-step cutover from the repository root:

```bash
docker build -t ninfer-4090:hybrid .

docker stop ninfer-qwen
docker run --gpus all --name ninfer-qwen -d \
    --restart unless-stopped \
    -p 127.0.0.1:8080:8080 \
    -v ninfer-continuations:/var/cache/ninfer \
    ninfer-4090:hybrid

# readiness and the new surfaces
curl -s http://127.0.0.1:8080/health          # {"status":"ok"}
curl -s http://127.0.0.1:8080/telemetry | head -c 400   # gpu/scheduler/cache/memory present
curl -sN --max-time 5 http://127.0.0.1:8080/events | head -5  # server_start + backlog
curl -s http://127.0.0.1:8080/ | head -c 100                  # dashboard index.html
```

Then check the dashboard at `http://127.0.0.1:8080/` and walk every panel: headline,
throughput, scheduler, GPU, energy (the energy panel shows "unavailable" only if this board
lacks the cumulative counter), VRAM, context cache, latency, slots, requests. Send one real
chat completion and confirm a `request_done` record lands in
`/var/cache/ninfer/request-log.jsonl`, appears on the Requests panel, and shows in the
dashboard's `/events` stream. The old image stays tagged `ninfer-4090:sm89`; rollback is:

```bash
docker stop ninfer-qwen && docker rm ninfer-qwen
docker run --gpus all --name ninfer-qwen -d --restart unless-stopped \
    -p 127.0.0.1:8080:8080 \
    -v ninfer-continuations:/var/cache/ninfer \
    ninfer-4090:sm89   # original fork CMD
```

Note for the window: the hybrid image uses the CUDA 13.1.2 runtime base (upstream) and keeps
the forward-compat libraries removed, so it needs the host driver through ordinary CUDA
compatibility, exactly as the current GeForce setup runs. The request log is append-only, so
the pre-existing `request-log.jsonl` lines from either build replay without confusion;
`server_start` records mark instance boundaries.
