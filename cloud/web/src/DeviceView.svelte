<script>
  import { get, ago } from './lib/api.js';
  import { live } from './lib/live.svelte.js';
  import Overview from './tabs/Overview.svelte';
  import Charts from './tabs/Charts.svelte';
  import Alarms from './tabs/Alarms.svelte';
  import Commands from './tabs/Commands.svelte';
  import Firmware from './tabs/Firmware.svelte';
  import Downloads from './tabs/Downloads.svelte';

  let { id, startTab = 'Overview' } = $props();
  let device = $state(null);
  let error = $state('');
  let tab = $state(startTab);
  const tabs = { Overview, Charts, Alarms, Commands, Firmware, Downloads };
  const Tab = $derived(tabs[tab]);

  $effect(() => {
    live.n;
    get(`/devices/${encodeURIComponent(id)}`).then((d) => ((device = d), (error = '')), (e) => (error = e.message));
  });
</script>

{#if error}<p class="err">{error}</p>{/if}
{#if device}
  <div class="row head">
    <h1>{device.name ?? device.id}</h1>
    <span class="badge" class:ok={device.online} class:bad={!device.online}>{device.online ? 'online' : 'offline'}</span>
    <span class="muted">last message {ago(device.last_seen)}</span>
    {#if device.fw_version}<span class="muted">firmware {device.fw_version}</span>{/if}
  </div>
  <nav class="row">
    {#each Object.keys(tabs) as t}
      <button class:primary={t === tab} onclick={() => ((tab = t), (location.hash = `${encodeURIComponent(id)}/${t}`))}>{t}</button>
    {/each}
  </nav>
  <section><Tab {device} /></section>
{/if}

<style>
  .head { margin-bottom: .8rem; }
  nav { margin-bottom: 1rem; }
  section { display: flex; flex-direction: column; gap: .8rem; }
</style>
