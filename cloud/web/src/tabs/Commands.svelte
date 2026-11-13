<script>
  import { get, post, fmtTime } from '../lib/api.js';
  import { live } from '../lib/live.svelte.js';
  import { auth } from '../lib/auth.svelte.js';

  let { device } = $props();
  // The settings the Gateway accepts from the cloud (Tier 2). Units are the firmware's fixed-point ones.
  const KEYS = {
    telemetry_interval_ms: 'ms, 1000..3600000',
    health_interval_ms: 'ms, 1000..3600000',
    polling_interval_ms: 'ms, 100..10000',
    modbus_poll_period_ms: 'ms, 100..60000',
    temp_alarm_high: 'centi-°C', temp_alarm_low: 'centi-°C', temp_hysteresis: 'centi-°C',
    humidity_alarm_high: 'centi-%', humidity_alarm_low: 'centi-%', humidity_hysteresis: 'centi-%',
    pressure_alarm_high: 'deci-hPa', pressure_alarm_low: 'deci-hPa', pressure_hysteresis: 'deci-hPa',
  };
  let key = $state('telemetry_interval_ms');
  let value = $state('');
  let rows = $state([]);
  let error = $state('');
  let busy = $state(false);
  const base = $derived(`/devices/${encodeURIComponent(device.id)}`);

  const load = () => get(`${base}/commands?limit=30`).then((r) => (rows = r), (e) => (error = e.message));
  $effect(() => {
    live.n;
    load();
  });
  $effect(() => {
    const t = setInterval(load, 5000); // a command with no answer turns into "timeout" without any event
    return () => clearInterval(t);
  });

  async function send(kind, body) {
    busy = true;
    error = '';
    try {
      await post(`${base}/${kind}`, body);
      await load();
    } catch (e) {
      error = e.message;
    } finally {
      busy = false;
    }
  }
  const sendConfig = (ev) => {
    ev.preventDefault();
    if (value === '' || !Number.isInteger(Number(value))) return (error = 'The value must be an integer.');
    send('config', { [key]: Number(value) });
  };
  // Device profiles (REQ-DM-100): which Field Devices the Gateway accepts, by Modbus address, register map and vendor code.
  let pSlave = $state('');
  let pMap = $state('1');
  let pVendor = $state('');
  let pDesc = $state('');
  const num = (s) => (s !== '' && Number.isInteger(Number(s)) ? Number(s) : null);
  const sendProfile = (ev, add) => {
    ev.preventDefault();
    const slave = num(pSlave);
    if (slave === null) return (error = 'The slave address must be an integer, 1..247.');
    if (!add) return send('control', { command: 'profile_remove', slave });
    const map = num(pMap);
    const vendor = num(pVendor);
    if (map === null || vendor === null) return (error = 'Map version and vendor code must be integers, 1..65535.');
    send('control', { command: 'profile_add', slave, map, vendor, ...(pDesc ? { desc: pDesc } : {}) });
  };
  const lastRestartAccepted = $derived(rows.find((r) => r.kind === 'control' && r.body.command === 'restart' && r.status === 'accepted'));
  const confirmable = $derived(lastRestartAccepted && Date.now() / 1000 - lastRestartAccepted.finished < 30);
  const cls = (s) => (s === 'applied' || s === 'accepted' ? 'ok' : s === 'pending' ? 'warn' : 'bad');
</script>

{#if auth.role !== 'admin'}<div class="card muted">This account can only read: sending commands needs an admin account.</div>{/if}
<fieldset disabled={auth.role !== 'admin'}>
<div class="card">
  <h2>Change a setting</h2>
  <form class="row" onsubmit={sendConfig}>
    <select bind:value={key}>{#each Object.keys(KEYS) as k}<option value={k}>{k}</option>{/each}</select>
    <input bind:value type="number" step="1" placeholder="value" />
    <span class="muted">{KEYS[key]}</span>
    <button class="primary" disabled={busy}>Send</button>
  </form>
  <p class="muted small">Other settings (filter, Modbus address, broker, NTP) can only be changed on the device itself.</p>
</div>

<div class="card">
  <h2>Device profiles</h2>
  <form class="row" onsubmit={(ev) => sendProfile(ev, true)}>
    <input bind:value={pSlave} type="number" min="1" max="247" placeholder="slave" />
    <input bind:value={pMap} type="number" min="1" max="65535" placeholder="map version" />
    <input bind:value={pVendor} type="number" min="1" max="65535" placeholder="vendor code" />
    <input bind:value={pDesc} maxlength="32" placeholder="label (optional)" />
    <button class="primary" disabled={busy}>Add or replace</button>
    <button type="button" class="danger" disabled={busy} onclick={(ev) => sendProfile(ev, false)}>Remove</button>
  </form>
  <p class="muted small">The Gateway polls only the Field Devices that have a profile. Removing the profile of the Field Device that is connected stops its polling until it is added again.</p>
</div>

<div class="card">
  <h2>Control</h2>
  <div class="row">
    <button disabled={busy} onclick={() => send('control', { command: 'read' })}>Read now</button>
    <button disabled={busy} onclick={() => send('control', { command: 'reset_metrics' })}>Reset metrics</button>
    <button disabled={busy} onclick={() => send('control', { command: 'fd_ack_alarm' })}>Acknowledge Field Device alarms</button>
    <button disabled={busy} onclick={() => send('control', { command: 'fd_reset_metrics' })}>Reset Field Device metrics</button>
    <button class="danger" disabled={busy} onclick={() => send('control', { command: 'restart' })}>Restart…</button>
    {#if confirmable}
      <button class="danger" disabled={busy} onclick={() => send('control', { command: 'restart_confirm' })}>Confirm restart (30 s window)</button>
    {/if}
  </div>
  <p class="muted small">A restart takes two steps: ask, then confirm within 30 s. The two Field Device buttons write the command register of the Field Device through the Gateway; the result says whether it answered.</p>
</div>
</fieldset>

{#if error}<p class="err">{error}</p>{/if}

<div class="card">
  <h2>Commands sent</h2>
  <table>
    <thead><tr><th>#</th><th>Sent</th><th>Command</th><th>Status</th><th>Reason</th></tr></thead>
    <tbody>
      {#each rows as r (r.id)}
        <tr>
          <td>{r.id}</td><td>{fmtTime(r.created)}</td>
          <td><code>{r.kind === 'control' ? r.body.command : JSON.stringify(r.body)}</code></td>
          <td><span class="badge {cls(r.status)}">{r.status}</span></td><td>{r.reason ?? ''}</td>
        </tr>
      {:else}
        <tr><td colspan="5" class="muted">Nothing sent yet.</td></tr>
      {/each}
    </tbody>
  </table>
</div>

<style> fieldset { border: 0; padding: 0; margin: 0; display: flex; flex-direction: column; gap: .8rem; min-width: 0; }
  .small { font-size: .8rem; margin: .6rem 0 0; } </style>
