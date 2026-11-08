<script>
  // A small line chart in SVG. points: [{t (epoch s), v (number | null)}]. Gaps (null) break the line.
  let { points = [], label = '', unit = '', color = 'var(--accent)', decimals = 1 } = $props();

  const W = 640, H = 170, L = 46, R = 10, T = 10, B = 24;
  const valid = $derived(points.filter((p) => p.v != null));
  const t0 = $derived(points.length ? points[0].t : 0);
  const t1 = $derived(points.length ? points[points.length - 1].t : 1);
  const lo = $derived(valid.length ? Math.min(...valid.map((p) => p.v)) : 0);
  const hi = $derived(valid.length ? Math.max(...valid.map((p) => p.v)) : 1);
  const pad = $derived((hi - lo) * 0.1 || 1);
  const x = (t) => L + ((t - t0) / (t1 - t0 || 1)) * (W - L - R);
  const y = (v) => T + (1 - (v - (lo - pad)) / (hi - lo + 2 * pad)) * (H - T - B);
  const path = $derived(
    points.map((p, i) => (p.v == null ? '' : `${i && points[i - 1].v != null ? 'L' : 'M'}${x(p.t).toFixed(1)},${y(p.v).toFixed(1)}`)).join(' ')
  );
  const ticks = $derived([lo, (lo + hi) / 2, hi]);
  const fmt = (v) => v.toFixed(decimals);
  const clock = (t) => new Date(t * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  const last = $derived(valid.length ? valid[valid.length - 1].v : null);
</script>

<div class="card">
  <div class="row" style="justify-content: space-between">
    <h2 style="margin:0">{label}</h2>
    <span class="muted">{last == null ? 'no data' : `${fmt(last)} ${unit}`}</span>
  </div>
  <svg viewBox="0 0 {W} {H}" width="100%" role="img" aria-label="{label} over time">
    {#each ticks as v}
      <line x1={L} x2={W - R} y1={y(v)} y2={y(v)} stroke="var(--line)" />
      <text x={L - 6} y={y(v) + 4} text-anchor="end" font-size="11" fill="var(--muted)">{fmt(v)}</text>
    {/each}
    {#if valid.length}
      <path d={path} fill="none" stroke={color} stroke-width="2" stroke-linejoin="round" />
      <text x={L} y={H - 6} font-size="11" fill="var(--muted)">{clock(t0)}</text>
      <text x={W - R} y={H - 6} text-anchor="end" font-size="11" fill="var(--muted)">{clock(t1)}</text>
    {/if}
  </svg>
</div>
