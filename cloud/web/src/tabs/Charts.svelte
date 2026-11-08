<script>
  import { get } from '../lib/api.js';
  import { live } from '../lib/live.svelte.js';
  import Chart from '../lib/Chart.svelte';

  let { device } = $props();
  const ranges = { '1 hour': 3600, '6 hours': 21600, '24 hours': 86400, '7 days': 604800 };
  let range = $state('1 hour');
  let rows = $state([]);
  let error = $state('');

  $effect(() => {
    live.n;
    const since = Math.floor(Date.now() / 1000) - ranges[range];
    get(`/devices/${encodeURIComponent(device.id)}/telemetry?since=${since}&limit=5000`).then(
      (r) => ((rows = r), (error = '')),
      (e) => (error = e.message)
    );
  });
  const series = (key, scale = 1) => rows.map((r) => ({ t: r.ts, v: r[key] == null ? null : r[key] / scale }));
</script>

<div class="row">
  <span class="muted">Range</span>
  {#each Object.keys(ranges) as r}<button class:primary={r === range} onclick={() => (range = r)}>{r}</button>{/each}
  <span class="muted">{rows.length} readings</span>
</div>
{#if error}<p class="err">{error}</p>{/if}
<Chart label="Temperature" unit="°C" series={[
  { label: 'gateway', color: 'var(--temp)', points: series('temperature_deci_c', 10) },
  { label: 'field device', color: 'var(--temp-fd)', dashed: true, points: series('fd_temperature_deci_c', 10) },
]} />
<Chart label="Humidity" unit="%" decimals={0} series={[
  { label: 'gateway', color: 'var(--hum)', points: series('humidity_pct') },
  { label: 'field device', color: 'var(--hum-fd)', dashed: true, points: series('fd_humidity_pct') },
]} />
<Chart label="Pressure" unit="hPa" decimals={0} series={[
  { label: 'gateway', color: 'var(--pres)', points: series('pressure_hpa') },
  { label: 'field device', color: 'var(--pres-fd)', dashed: true, points: series('fd_pressure_hpa') },
]} />
