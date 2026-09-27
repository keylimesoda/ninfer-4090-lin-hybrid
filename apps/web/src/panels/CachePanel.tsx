import { Legend, StackedBar } from '../components/charts'
import { CHART } from '../components/echart'
import { Term, Tooltip } from '../components/tooltip'
import { Empty, Meter, Panel, Pill, Stat } from '../components/ui'
import { REUSE_PATH_COLOR, type RequestSummary } from '../lib/derive'
import { bytes, count, percent } from '../lib/format'
import { GLOSSARY, type GlossaryKey } from '../lib/glossary'
import type { ContextCacheTelemetry } from '../lib/telemetry'

function Occupancy({
  name,
  hint,
  used,
  capacity,
  display,
  color,
}: {
  name: string
  hint: GlossaryKey
  used: number
  capacity: number
  display: string
  color: string
}) {
  const fraction = capacity === 0 ? 0 : used / capacity
  const tone = fraction > 0.95 ? 'danger' : fraction > 0.85 ? 'warning' : 'neutral'
  return (
    <div className="cache__tier">
      <div className="cache__tier-head">
        <span className="cache__tier-name">
          <Term k={hint}>{name}</Term>
        </span>
        <span className="cache__tier-fill">{display}</span>
      </div>
      <Meter
        fraction={fraction}
        color={tone === 'danger' ? 'var(--danger)' : tone === 'warning' ? 'var(--warning)' : color}
      />
    </div>
  )
}

export function CachePanel({
  cache,
  summary,
  replay,
}: {
  cache: ContextCacheTelemetry | undefined
  summary: RequestSummary
  replay: boolean
}) {
  if (!cache) {
    return (
      <Panel title="Context cache" hint="contextCache">
        <Empty>
          {replay ? 'no throughput record in the loaded log' : 'engine not attached'}
        </Empty>
      </Panel>
    )
  }

  const hostStateSaturated =
    cache.host_state_capacity_slots > 0 && cache.host_state_occupied_slots / cache.host_state_capacity_slots > 0.95
  const hostKvSaturated =
    cache.host_kv_capacity_bytes > 0 && cache.host_kv_occupied_bytes / cache.host_kv_capacity_bytes > 0.95
  const saturated: string[] = []
  if (hostStateSaturated) saturated.push('host state')
  if (hostKvSaturated) saturated.push('host kv')

  const reuseSegments = Object.entries(summary.byReusePath)
    .filter(([, count]) => count > 0)
    .sort((a, b) => b[1] - a[1])
    .map(([path, count]) => ({
      label: path,
      value: count,
      color: REUSE_PATH_COLOR[path] ?? CHART.dim,
      hint: GLOSSARY.reusePath.body,
      display: `${count} of ${summary.count} requests`,
    }))

  const pressureRows: Array<[string, number, string]> = [
    ['spill pages', cache.pressure.spill_pages, GLOSSARY.pressureActions.body],
    [
      'cow tails',
      cache.pressure.partial_tail_cow_pages,
      'Pages split so only the divergent tail was re-copied; the shared prefix stayed resident.',
    ],
    ['owners degraded', cache.pressure.private_owners_degraded + cache.pressure.shared_owners_degraded, GLOSSARY.ownerDegradation.body],
    ['owners evicted', cache.pressure.private_owners_evicted + cache.pressure.shared_owners_evicted, 'Owners dropped entirely: their state is gone and the next use recomputes.'],
    ['checkpoints dropped', cache.pressure.checkpoints_dropped, 'Rewrite checkpoints discarded under pressure; a later turn diverges from an earlier point and recomputes more.'],
    [
      'budget exhaustion',
      cache.pressure.search_budget_exhaustions,
      'Pressure searches that ran out of planning budget before finding a safe plan.',
    ],
    ['maximal fallbacks', cache.pressure.maximal_fallback_selections, 'Selections that could only fit the largest candidate, the worst-case plan.'],
  ]

  const transfers: Array<[string, number, number]> = [
    ['state d2h', cache.state.d2h.bytes, cache.state.d2h.seconds],
    ['state h2d', cache.state.h2d.bytes, cache.state.h2d.seconds],
    ['main kv d2h', cache.main_kv.d2h.bytes, cache.main_kv.d2h.seconds],
    ['main kv h2d', cache.main_kv.h2d.bytes, cache.main_kv.h2d.seconds],
    ['backend kv h2d', cache.backend_kv.h2d.bytes, cache.backend_kv.h2d.seconds],
  ]

  return (
    <Panel
      title="Context cache"
      hint="contextCache"
      note={
        saturated.length > 0 ? (
          <Pill tone="warning">{saturated.join(' · ')} at capacity</Pill>
        ) : undefined
      }
    >
      <div className="stat-row">
        <Stat
          value={percent(summary.prefillAvoided)}
          label="prefill avoided"
          hint="prefillAvoided"
          tone={summary.prefillAvoided > 0.5 ? 'accent' : 'neutral'}
        />
        <Stat
          value={count(cache.host_state_occupied_slots)}
          label="host state"
          hint="stateSlots"
          tone={hostStateSaturated ? 'danger' : 'neutral'}
        />
        <Stat
          value={bytes(cache.host_kv_occupied_bytes)}
          label="host kv"
          hint="hostKv"
          tone={hostKvSaturated ? 'danger' : 'neutral'}
        />
        <Stat
          value={count(cache.pressure.spill_pages)}
          label="spilled"
          hint="pressureActions"
          tone={cache.pressure.spill_pages > 0 ? 'warning' : 'neutral'}
        />
        <Stat
          value={count(cache.shared_active_references)}
          label="shared refs"
          hint="contextCache"
        />
      </div>

      <div className="cache__tiers">
        <Occupancy
          name="host state"
          hint="stateSlots"
          used={cache.host_state_occupied_slots}
          capacity={cache.host_state_capacity_slots}
          display={`${count(cache.host_state_occupied_slots)} / ${count(cache.host_state_capacity_slots)} slots · ${count(cache.device_state_occupied_slots)} on device`}
          color={CHART.blue}
        />
        <Occupancy
          name="host kv"
          hint="hostKv"
          used={cache.host_kv_occupied_bytes}
          capacity={cache.host_kv_capacity_bytes}
          display={`${bytes(cache.host_kv_occupied_bytes)} / ${bytes(cache.host_kv_capacity_bytes)} (${percent(cache.host_kv_capacity_bytes === 0 ? 0 : cache.host_kv_occupied_bytes / cache.host_kv_capacity_bytes)})`}
          color={CHART.accent}
        />
      </div>

      {summary.count === 0 ? (
        <Empty>no completed requests in the retained window</Empty>
      ) : (
        <div className="cache__sources">
          <div className="eyebrow">
            <Term k="reuseBreakdown">where prefixes came from</Term> ({summary.count} requests)
          </div>
          <StackedBar segments={reuseSegments} />
          <Legend items={reuseSegments.map((s) => ({ label: s.label, color: s.color, hint: s.hint }))} />
        </div>
      )}

      <table className="table">
        <tbody>
          {pressureRows
            .filter(([, value]) => value > 0)
            .map(([label, value, hint]) => (
              <tr key={label}>
                <td>
                  <Tooltip title={label} body={hint} className="tip--term">
                    {label}
                  </Tooltip>
                </td>
                <td className="numeric emphasis">{count(value)}</td>
              </tr>
            ))}
          {transfers
            .filter(([, bytesMoved]) => bytesMoved > 0)
            .map(([label, bytesMoved, seconds]) => (
              <tr key={label}>
                <td>
                  <Tooltip title={label} body={GLOSSARY.transfer.body} className="tip--term">
                    {label}
                  </Tooltip>
                </td>
                <td className="numeric emphasis">
                  {bytes(bytesMoved)}{seconds > 0.001 ? ` · ${seconds.toFixed(2)}s` : ''}
                </td>
              </tr>
            ))}
        </tbody>
      </table>

      <p className="panel__footnote">
        <Term k="pressureActions">Pressure</Term> is the cache staying inside its budget, not a
        failure: a spill moves state to host where the next reuse imports it back, while an
        eviction or a dropped checkpoint is state that is gone and must be recomputed. The device
        side reads <Term k="kvPages">{count(cache.device_main_kv_occupied_pages)} main +{' '}
        {count(cache.device_backend_kv_occupied_pages)} backend pages</Term> resident against the{' '}
        {count(cache.kv_capacity_page_groups)}-group budget.
      </p>
    </Panel>
  )
}