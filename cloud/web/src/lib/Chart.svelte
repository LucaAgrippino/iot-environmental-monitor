<script>
  // A small line chart in SVG with one or more series on the same scale, so that two sensors can be compared.
  // series: [{ label, color, dashed, points: [{t (epoch s), v (number | null)}] }]. Gaps (null) break the line.
  let { series = [], label = '', unit = '', decimals = 1 } = $props();

  const W = 640, H = 170, L = 46, R = 10, T = 10, B = 24;
  const all = $derived(series.flatMap((s) => s.points));
  const valid = $derived(all.filter((p) => p.v != null));
  const t0 = $derived(all.length ? Math.min(...all.map((p) => p.t)) : 0);
  const t1 = $derived(all.length ? Math.max(...all.map((p) => p.t)) : 1);
  const lo = $derived(valid.length ? Math.min(...valid.map((p) => p.v)) : 0);
  const hi = $derived(valid.length ? Math.max(...valid.map((p) => p.v)) : 1);
  const pad = $derived((hi - lo) * 0.1 || 1);
  const x = (t) => L + ((t - t0) / (t1 - t0 || 1)) * (W - L - R);
  const y = (v) => T + (1 - (v - (lo - pad)) / (hi - lo + 2 * pad)) * (H - T - B);
  const path = (pts) =>
    pts.map((p, i) => (p.v == null ? '' : `${i && pts[i - 1].v != null ? 'L' : 'M'}${x(p.t).toFixed(1)},${y(p.v).toFixed(1)}`)).join(' ');
  // labels sit at the bottom, middle and top of the axis, so a constant signal still gets three distinct labels
  const ticks = $derived([lo - pad, (lo + hi) / 2, hi + pad]);
  // a whole-number unit over a tiny range would print the same label three times: add a decimal
  const nd = $derived(decimals === 0 && hi - lo + 2 * pad < 4 ? 1 : decimals);
  const fmt = (v) => v.toFixed(nd);
  const clock = (t) => new Date(t * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  const lastOf = (s) => {
    const v = s.points.filter((p) => p.v != null);
    return v.length ? v[v.length - 1].v : null;
  };
</script>

<div class="card">
  <div class="row" style="justify-content: space-between">
    <h2 style="margin:0">{label}</h2>
    <span class="row legend">
      {#each series as s}
        <span class="key">
          <svg width="22" height="8" aria-hidden="true">
            <line x1="0" x2="22" y1="4" y2="4" stroke={s.color} stroke-width="2" stroke-dasharray={s.dashed ? '4 3' : 'none'} />
          </svg>
          {s.label}: <b>{lastOf(s) == null ? 'no data' : `${fmt(lastOf(s))} ${unit}`}</b>
        </span>
      {/each}
    </span>
  </div>
  <svg viewBox="0 0 {W} {H}" width="100%" role="img" aria-label="{label} over time">
    {#each ticks as v}
      <line x1={L} x2={W - R} y1={y(v)} y2={y(v)} stroke="var(--line)" />
      <text x={L - 6} y={y(v) + 4} text-anchor="end" font-size="11" fill="var(--muted)">{fmt(v)}</text>
    {/each}
    {#if valid.length}
      {#each series as s}
        <path d={path(s.points)} fill="none" stroke={s.color} stroke-width="2" stroke-linejoin="round"
          stroke-dasharray={s.dashed ? '6 4' : 'none'} />
      {/each}
      <text x={L} y={H - 6} font-size="11" fill="var(--muted)">{clock(t0)}</text>
      <text x={W - R} y={H - 6} text-anchor="end" font-size="11" fill="var(--muted)">{clock(t1)}</text>
    {/if}
  </svg>
</div>

<style>
  .legend { gap: 1rem; font-size: .85rem; color: var(--muted); }
  .key { display: inline-flex; align-items: center; gap: .35rem; }
  .key b { color: var(--ink); font-weight: 600; font-variant-numeric: tabular-nums; }
</style>
