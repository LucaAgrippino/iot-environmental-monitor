<script>
  import { get, fmtTime } from '../lib/api.js';
  import { live } from '../lib/live.svelte.js';
  let { device } = $props();
  let rows = $state([]);
  $effect(() => {
    live.n;
    get(`/devices/${encodeURIComponent(device.id)}/alarms?limit=200`).then((r) => (rows = r.reverse()));
  });
  const value = (r, v) => (r.sensor_id === 'temperature' ? (v / 100).toFixed(1) + ' °C' : v);
</script>

<div class="card">
  <h2>Alarm history</h2>
  <table>
    <thead><tr><th>Time</th><th>Sensor</th><th>Type</th><th class="num">Measured</th><th class="num">Threshold</th><th>Source</th></tr></thead>
    <tbody>
      {#each rows as r}
        <tr>
          <td>{fmtTime(r.ts)}</td><td>{r.sensor_id}</td>
          <td><span class="badge" class:bad={r.alarm_type !== 'CLEAR'} class:ok={r.alarm_type === 'CLEAR'}>{r.alarm_type}</span></td>
          <td class="num">{value(r, r.measured_value_raw)}</td><td class="num">{value(r, r.threshold_value_raw)}</td><td>{r.source}</td>
        </tr>
      {:else}
        <tr><td colspan="6" class="muted">No alarms.</td></tr>
      {/each}
    </tbody>
  </table>
</div>
