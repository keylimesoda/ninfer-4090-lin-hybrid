import { Legend, SampleChart, StackedBar } from '../components/charts'
import { CHART } from '../components/echart'
import { Info, Term } from '../components/tooltip'
import { Empty, Meter, Panel, Stat } from '../components/ui'
import { count, percent, seconds } from '../lib/format'
import { GLOSSARY } from '../lib/glossary'
import type { ServerStartRecord, ThroughputRecord } from '../lib/records'
import { latest, samples } from '../lib/series'
import type { Telemetry } from '../lib/telemetry'

// The five host phases the engine names, in the order the execution thread moves through them.
// Device wait is reported beside them, not inside the bar: it is the thread parked, not work.
const WORKER_PHASES = [
  { key: 'engine_boundary', label: 'boundary', color: CHART.muted, hint: GLOSSARY.workerBoundary.body },
  { key: 'program_submit', label: 'submit', color: CHART.blue, hint: GLOSSARY.workerSubmit.body },
  { key: 'program_post', label: 'post', color: CHART.violet, hint: GLOSSARY.workerPost.body },
  { key: 'engine_commit_output', label: 'commit', color: CHART.accent, hint: GLOSSARY.workerCommit.body },
  { key: 'engine_maintenance', label: 'upkeep', color: CHART.dim, hint: GLOSSARY.workerMaintenance.body },
] as const

export function SchedulerPanel({
  telemetry,
  records,
  engine,
}: {
  telemetry: Telemetry | null
  records: ThroughputRecord[]
  /** Configuration from the log's own server_start, used when no live telemetry is attached. */
  engine: ServerStartRecord['engine'] | undefined
}) {
  const scheduler = telemetry?.scheduler
  const sample = latest(records)
  const running = scheduler?.running ?? sample?.scheduler.running ?? 0
  const waiting = scheduler?.waiting ?? sample?.scheduler.waiting ?? 0
  const lanes = scheduler?.max_concurrency ?? engine?.max_concurrency ?? 1
  const pending = scheduler?.max_pending_requests ?? engine?.max_pending_requests ?? 0

  const worker = scheduler?.worker_seconds
  const detail = scheduler?.worker_detail
  const workerTotal = worker
    ? WORKER_PHASES.reduce((total, phase) => total + worker[phase.key], 0)
    : 0

  const abandoned = scheduler?.decode_rounds_abandoned ?? 0
  const rounds = scheduler?.decode_rounds ?? sample?.decode_batch.rounds ?? 0
  const rowRounds = scheduler?.decode_row_rounds ?? sample?.decode_batch.row_rounds ?? 0

  return (
    <Panel
      title="Scheduler"
      note={`${lanes} lane${lanes === 1 ? '' : 's'} · ${pending} pending slots`}
    >
      <div className="stat-row">
        <Stat
          value={`${running}/${lanes}`}
          label="running"
          hint="lanes"
          tone={running >= lanes ? 'warning' : 'accent'}
        />
        <Stat
          value={`${waiting}`}
          label="waiting"
          hint="queued"
          tone={waiting >= pending && pending > 0 ? 'danger' : waiting > 0 ? 'warning' : 'neutral'}
        />
        <Stat
          value={count(rounds)}
          label="decode rounds"
          hint="decodeRounds"
        />
        <Stat
          value={count(rowRounds)}
          label="row rounds"
          hint="decodeBatch"
        />
        <Stat
          value={count(abandoned)}
          label="abandoned"
          hint="decodeRounds"
          tone={abandoned > 0 ? 'danger' : 'neutral'}
        />
      </div>

      <div className="scheduler__occupancy">
        <div className="eyebrow">
          <Term k="laneOccupancy">lane occupancy</Term>
        </div>
        <Meter fraction={lanes === 0 ? 0 : running / lanes} />
        <div className="eyebrow">
          <Term k="ingressQueue">ingress queue</Term>
        </div>
        <Meter
          fraction={pending === 0 ? 0 : waiting / pending}
          color={waiting >= pending ? 'var(--danger)' : 'var(--warning)'}
        />
      </div>

      {records.length === 0 ? (
        <Empty>no scheduler samples yet</Empty>
      ) : (
        <SampleChart
          label="Running requests and queue depth at each report"
          series={[
            {
              name: 'waiting',
              samples: samples(records, (record) => record.scheduler.waiting),
              color: CHART.warning,
            },
            {
              name: 'running',
              samples: samples(records, (record) => record.scheduler.running),
              color: CHART.blue,
            },
          ]}
          ceiling={lanes}
          legend={
            <Legend
              items={[
                { label: 'running', color: CHART.blue, hint: GLOSSARY.lanes.body },
                { label: 'waiting', color: CHART.warning, hint: GLOSSARY.queued.body },
              ]}
            />
          }
          caption={
            <>
              snapshots, not interval means <Info k="snapshotSeries" />
            </>
          }
        />
      )}

      {worker && workerTotal > 0 ? (
        <div className="scheduler__worker">
          <div className="eyebrow">
            <Term k="workerSplit">execution thread wall clock</Term>
          </div>
          <StackedBar
            segments={WORKER_PHASES.map((phase) => ({
              label: phase.label,
              value: worker[phase.key],
              color: phase.color,
              hint: phase.hint,
              display: `${seconds(worker[phase.key])} · ${percent(worker[phase.key] / workerTotal)} of thread time`,
            }))}
          />
          <Legend
            items={WORKER_PHASES.map((phase) => ({
              label: phase.label,
              color: phase.color,
              hint: phase.hint,
            }))}
          />
          <p className="panel__footnote">
            One mutex serializes these phases, so a second spent in any of them is a second no
            resident lane advances. Prefill and decode work run inside submit and post; a rising
            boundary or upkeep share with steady compute is bookkeeping, not model speed.{' '}
            {WORKER_PHASES.map((phase) => `${phase.label} ${percent(worker[phase.key] / workerTotal)}`).join(
              ' · ',
            )}
            {worker.device_wait > 0 ? (
              <>
                {' '}
                <Term k="workerDeviceWait">device wait</Term> {seconds(worker.device_wait)}
              </>
            ) : null}
          </p>
          {detail ? (
            <p className="panel__footnote">
              <Term k="workerDetail">Slow paths</Term>: admission{' '}
              {seconds(detail.admission_policy.seconds)} over{' '}
              {count(detail.admission_policy.invocations)} calls · context{' '}
              {seconds(detail.context_progress.seconds)} over{' '}
              {count(detail.context_progress.invocations)} · stats{' '}
              {seconds(detail.stats_publication.seconds)} over{' '}
              {count(detail.stats_publication.invocations)}
            </p>
          ) : null}
        </div>
      ) : null}
    </Panel>
  )
}