<script>
  import { get, ago } from './lib/api.js';
  import { live, startLive, stopLive } from './lib/live.svelte.js';
  import DeviceView from './DeviceView.svelte';
  import Login from './Login.svelte';
  import Alerts from './Alerts.svelte';
  import { auth, whoami, logout } from './lib/auth.svelte.js';

  let devices = $state([]);
  let error = $state('');
  const parts = decodeURIComponent(location.hash.slice(1)).split('/'); // #<device>/<tab>: links can point at a tab
  let page = $state(parts[0] === 'alerts' ? 'alerts' : 'devices');
  let selected = $state(parts[0] === 'alerts' ? '' : parts[0]);
  const startTab = parts[1] || 'Overview';

  async function load() {
    try {
      devices = await get('/devices');
      error = '';
      if (!selected && page === 'devices' && devices.length) select(devices[0].id);
    } catch (e) {
      error = e.message;
    }
  }
  function select(id) {
    page = 'devices';
    selected = id;
    location.hash = encodeURIComponent(id);
  }

  whoami();
  $effect(() => {
    if (auth.user) startLive(); // the stream needs the session cookie
    else stopLive();
  });
  $effect(() => {
    live.n; // a message arrived: refresh the list (online state, latest values, alarms)
    if (auth.user) load();
  });
  $effect(() => {
    const t = setInterval(() => auth.user && load(), 15000); // also catches a device going offline, which sends no event
    return () => clearInterval(t);
  });
</script>

{#if !auth.checked}
  <p class="muted" style="padding:1rem">Loading…</p>
{:else if !auth.user}
  <Login />
{:else}
<div class="shell">
  <aside>
    <h1>Environmental Monitor</h1>
    <div class="muted conn">
      <span class="dot" class:on={live.connected}></span>{live.connected ? 'live' : 'reconnecting'}
    </div>
    {#each devices as d (d.id)}
      <button class="dev" class:sel={d.id === selected} onclick={() => select(d.id)}>
        <span class="dot" class:on={d.online}></span>
        <span class="name">{d.name ?? d.id}</span>
        {#if d.active_alarms.length}<span class="badge bad">{d.active_alarms.length}</span>{/if}
        <span class="muted small">{d.online ? 'online' : ago(d.last_seen)}</span>
      </button>
    {/each}
    <button class="dev" class:sel={page === 'alerts'} onclick={() => ((page = 'alerts'), (location.hash = 'alerts'))}>
      <span class="name">Alerts</span>
    </button>
    {#if !devices.length && !error}<p class="muted">No device has sent anything yet.</p>{/if}
    {#if error}<p class="err">{error}</p>{/if}
    {#if auth.requiresLogin}
      <div class="me muted">{auth.user} ({auth.role}) <button onclick={logout}>Sign out</button></div>
    {/if}
  </aside>
  <main>
    {#if page === 'alerts'}
      <Alerts />
    {:else if selected}
      {#key selected}<DeviceView id={selected} {startTab} />{/key}
    {/if}
  </main>
</div>
{/if}

<style>
  .shell { display: grid; grid-template-columns: 260px 1fr; min-height: 100vh; }
  aside { background: var(--panel); border-right: 1px solid var(--line); padding: 1rem; display: flex; flex-direction: column; gap: .4rem; }
  aside h1 { font-size: 1.1rem; }
  main { padding: 1rem 1.2rem; min-width: 0; }
  .me { margin-top: auto; font-size: .85rem; display: flex; gap: .5rem; align-items: center; flex-wrap: wrap; }
  .conn { font-size: .8rem; margin-bottom: .6rem; }
  .dot { display: inline-block; width: 9px; height: 9px; border-radius: 50%; background: var(--muted); margin-right: .45rem; }
  .dot.on { background: var(--ok); }
  .dev { display: flex; align-items: center; gap: .3rem; text-align: left; width: 100%; flex-wrap: wrap; }
  .dev.sel { border-color: var(--accent); background: color-mix(in srgb, var(--accent) 10%, var(--panel)); }
  .name { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .small { font-size: .75rem; width: 100%; padding-left: 1.1rem; }
  @media (max-width: 760px) {
    .shell { grid-template-columns: 1fr; }
    aside { border-right: 0; border-bottom: 1px solid var(--line); }
  }
</style>
