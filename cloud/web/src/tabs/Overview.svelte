<script>
  import { fmtTime } from '../lib/api.js';
  let { device } = $props();
  const l = $derived(device.latest);
  const h = $derived(device.health);
  const dc = (v) => (v == null ? '-' : (v / 10).toFixed(1));
  // health fields shown as a table: the scalar ones, in the order the firmware sends them
  const healthRows = $derived(h ? Object.entries(h).filter(([k, v]) => typeof v !== 'object' && !['schema_version', 'device_id'].includes(k)) : []);
</script>

{#if device.active_alarms.length}
  <div class="card">
    <h2>Active alarms</h2>
    {#each device.active_alarms as a}
      <div><span class="badge bad">{a.alarm_type}</span> {a.sensor_id} <span class="muted">since {fmtTime(a.ts)}</span></div>
    {/each}
  </div>
{/if}

<div class="grid">
  <div class="card"><div class="muted">Temperature (gateway)</div><div class="big">{dc(l?.temperature_deci_c)} °C</div></div>
  <div class="card"><div class="muted">Humidity</div><div class="big">{l?.humidity_pct ?? '-'} %</div></div>
  <div class="card"><div class="muted">Pressure</div><div class="big">{l?.pressure_hpa ?? '-'} hPa</div></div>
  <div class="card">
    <div class="muted">Field device {l?.fd_online ? '' : '(offline)'}</div>
    <div class="big">{dc(l?.fd_temperature_deci_c)} °C</div>
    <div class="muted">{l?.fd_humidity_pct ?? '-'} % · {l?.fd_pressure_hpa ?? '-'} hPa</div>
  </div>
</div>
{#if l && l.accel_x_mg != null}
  <div class="grid" data-testid="motion">
    <div class="card"><div class="muted">Accelerometer (mg)</div><div class="mid">{l.accel_x_mg} · {l.accel_y_mg} · {l.accel_z_mg}</div><div class="muted">x · y · z</div></div>
    <div class="card"><div class="muted">Gyroscope (mdps)</div><div class="mid">{l.gyro_x_mdps} · {l.gyro_y_mdps} · {l.gyro_z_mdps}</div><div class="muted">x · y · z</div></div>
    <div class="card"><div class="muted">Magnetometer (mgauss)</div><div class="mid">{l.mag_x_mgauss} · {l.mag_y_mgauss} · {l.mag_z_mgauss}</div><div class="muted">x · y · z</div></div>
  </div>
{/if}
{#if l}<div class="muted">Reading from {fmtTime(l.ts)}</div>{/if}

{#if healthRows.length}
  <div class="card">
    <h2>Health</h2>
    <div class="cols">
      {#each healthRows as [k, v]}
        <div class="kv"><span class="muted">{k}</span><span>{typeof v === 'boolean' ? (v ? 'yes' : 'no') : v}</span></div>
      {/each}
    </div>
  </div>
{/if}

<style>
  .mid { font-size: 1.15rem; font-weight: 600; font-variant-numeric: tabular-nums; }
  .big { font-size: 1.8rem; font-weight: 600; font-variant-numeric: tabular-nums; }
  .cols { display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); gap: 0 1.5rem; }
  .kv { display: flex; justify-content: space-between; gap: 1rem; border-bottom: 1px solid var(--line); padding: .2rem 0; font-size: .9rem; }
</style>
