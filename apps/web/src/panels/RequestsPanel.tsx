import { StackedBar } from '../components/charts'
import { CHART } from '../components/echart'
import { Term, Tooltip } from '../components/tooltip'
import { Empty, Panel, Pill } from '../components/ui'
import { REUSE_PATH_COLOR } from '../lib/derive'
import { clock, count, percent, seconds } from '../lib/format'
import { GLOSSARY } from '../lib/glossary'
import type { RequestDoneRecord, RequestStartRecord } from '../lib/records'

const PHASES = [
  {
    key: 'queue',
    color: CHART.warning,
    hint: 'Waiting in the FIFO before a lane was available.',
  },
  { key: 'prefill', color: CHART.violet, hint: 'Evaluating prompt tokens no prefix covered.' },
  {
    key: 'decode',
    color: CHART.accent,
    hint: 'Generating tokens once the first one had been emitted.',
  },
] as const

/** The reuse-path values the engine reports, and what each one means for that request. */
const REUSE_PATH_HINT: Record<string, string> = {
  root: 'Nothing was reusable: the whole prompt was prefilled from zero.',
  private_endpoint: GLOSSARY.reusePath.body,
  private_turn_closure: GLOSSARY.reusePath.body,
  private_response_replay: GLOSSARY.reusePath.body,
  private_long_anchor: GLOSSARY.reusePath.body,
  shared_stable_prefix: GLOSSARY.reusePath.body,
}

/** Per-request time split, drawn on the same phase colors the latency panel uses. */
function Waterfall({ record }: { record: RequestDoneRecord }) {
  const queue = record.engine_timing.queue_wait_seconds
  const t = record.timings_seconds
  const total = queue + t.prefill + t.decode
  const values: Record<string, number> = { queue, prefill: t.prefill, decode: t.decode }
  return (
    <StackedBar
      height={5}
      segments={PHASES.map((phase) => ({
        label: phase.key,
        value: values[phase.key],
        color: phase.color,
        hint: phase.hint,
        display: `${seconds(values[phase.key])} of ${seconds(total)}`,
      }))}
    />
  )
}

/** Everything the record knows about one request, shown on its timestamp. */
function detail(record: RequestDoneRecord) {
  const spec = record.speculative
  const lines: string[] = [
    `${record.request.protocol} · ${record.request.request_id}`,
    `finish: ${record.result.finish_reason}`,
    `reuse path: ${record.result.prefix_reuse_path}`,
    `reused ${count(record.result.prefix_cache_hit_tokens)} of ${count(
      record.result.prompt_tokens,
    )} prompt tokens`,
    `computed ${count(record.result.computed_prefill_tokens)} prefill tokens`,
  ]
  if (spec && spec.rounds > 0) {
    lines.push(
      `mtp: ${spec.accepted_tokens}/${spec.drafted_tokens} accepted (${percent(
        spec.drafted_tokens === 0 ? 0 : spec.accepted_tokens / spec.drafted_tokens,
      )})`,
    )
  }
  return lines.join('\n')
}

export function RequestsPanel({
  requests,
  active,
}: {
  requests: RequestDoneRecord[]
  active: RequestStartRecord[]
}) {
  const recent = [...requests].reverse().slice(0, 14)

  return (
    <Panel
      title="Requests"
      hint="ttftSplit"
      className="panel--wide"
      note={
        active.length > 0 ? (
          <Pill tone="accent">{active.length} in flight</Pill>
        ) : (
          `${requests.length} retained`
        )
      }
    >
      {recent.length === 0 ? (
        <Empty>no completed requests yet</Empty>
      ) : (
        <table className="table">
          <thead>
            <tr>
              <th>at</th>
              <th>proto</th>
              <th className="numeric">prompt</th>
              <th className="numeric">gen</th>
              <th>
                <Term k="reusePath">src</Term>
              </th>
              <th className="numeric">
                <Term k="ttft">ttft</Term>
              </th>
              <th className="numeric">
                <Term k="perSequenceRate">tok/s</Term>
              </th>
              <th style={{ width: '22%' }}>
                <Term k="ttftSplit">phases</Term>
              </th>
            </tr>
          </thead>
          <tbody>
            {recent.map((record) => {
              const path = record.result.prefix_reuse_path
              const decodeRate =
                record.timings_seconds.decode > 0.01
                  ? record.result.completion_tokens / record.timings_seconds.decode
                  : 0
              return (
                <tr key={`${record.request.request_id}-${record.timestamp_unix_ms}`}>
                  <td>
                    <Tooltip
                      title={clock(record.timestamp_unix_ms)}
                      body={<span className="tipwrap">{detail(record)}</span>}
                      className="tip--term"
                    >
                      {clock(record.timestamp_unix_ms)}
                    </Tooltip>
                  </td>
                  <td>{record.request.protocol.replace('openai_', '')}</td>
                  <td className="numeric emphasis">{count(record.result.prompt_tokens)}</td>
                  <td className="numeric emphasis">{count(record.result.completion_tokens)}</td>
                  <td style={{ color: REUSE_PATH_COLOR[path] }}>
                    <Tooltip
                      title={path}
                      body={REUSE_PATH_HINT[path] ?? GLOSSARY.reusePath.body}
                      className="tip--term"
                    >
                      {path}
                    </Tooltip>
                  </td>
                  <td className="numeric emphasis">{seconds(record.timings_seconds.ttft)}</td>
                  <td className="numeric">{decodeRate > 0 ? decodeRate.toFixed(0) : '—'}</td>
                  <td>
                    <Waterfall record={record} />
                  </td>
                </tr>
              )
            })}
          </tbody>
        </table>
      )}
      <p className="panel__footnote">
        Phase bars are proportional to each request&apos;s own total, coloured queue / prefill /
        decode. Hover a timestamp for that request&apos;s reuse path and MTP result.
      </p>
    </Panel>
  )
}