<script>
  import { get, post, put, del, fmtTime } from './lib/api.js';
  import { live } from './lib/live.svelte.js';
  import { auth } from './lib/auth.svelte.js';

  const MASK = '********';
  let channels = $state([]);
  let events = $state([]);
  let log = $state([]);
  let error = $state('');
  let notice = $state('');
  let busy = $state(false);
  let editing = $state(null); // null = form closed, 0 = new channel, n = editing channel n
  let form = $state(blank());

  function blank(kind = 'ntfy') {
    return {
      name: '', kind, enabled: true, events: ['alarm', 'device_offline', 'ota_result'],
      cfg: { url: 'https://ntfy.sh', topic: '', token: '', secret: '', host: '', port: '', security: 'starttls', username: '', password: '', from: '', to: '' },
    };
  }

  async function load() {
    try {
      [channels, events, log] = await Promise.all([get('/notify/channels'), get('/notify/events'), get('/notify/log?limit=50')]);
    } catch (e) {
      error = e.message;
    }
  }
  $effect(() => {
    live.n; // delivery attempts are logged as the events happen
    load();
  });

  function open(ch) {
    notice = error = '';
    if (!ch) return ((form = blank()), (editing = 0));
    const c = ch.config;
    form = { name: ch.name, kind: ch.kind, enabled: ch.enabled, events: [...ch.events],
      cfg: { ...blank(ch.kind).cfg, ...c, to: Array.isArray(c.to) ? c.to.join(', ') : '', port: c.port ?? '' } };
    editing = ch.id;
  }

  function payload() {
    const c = form.cfg;
    const config = form.kind === 'ntfy' ? { url: c.url, topic: c.topic, token: c.token || undefined }
      : form.kind === 'webhook' ? { url: c.url, secret: c.secret || undefined }
      : { host: c.host, port: c.port ? Number(c.port) : undefined, security: c.security, username: c.username || undefined,
          password: c.password || undefined, from: c.from, to: c.to };
    return { name: form.name, kind: form.kind, enabled: form.enabled, events: form.events, config };
  }

  async function run(fn) {
    busy = true;
    error = notice = '';
    try {
      await fn();
      await load();
    } catch (e) {
      error = e.message;
    } finally {
      busy = false;
    }
  }
  const save = (ev) => {
    ev.preventDefault();
    run(async () => {
      editing === 0 ? await post('/notify/channels', payload()) : await put(`/notify/channels/${editing}`, payload());
      editing = null;
    });
  };
  const toggle = (ch) => run(() => put(`/notify/channels/${ch.id}`, { enabled: !ch.enabled }));
  const remove = (ch) => confirm(`Delete channel "${ch.name}"?`) && run(() => del(`/notify/channels/${ch.id}`));
  const test = (ch) => run(async () => {
    const r = await post(`/notify/channels/${ch.id}/test`);
    if (r.status === 'sent') notice = `"${ch.name}": test message sent.`;
    else error = `"${ch.name}": ${r.error}`;
  });
  const flip = (name) => (form.events = form.events.includes(name) ? form.events.filter((e) => e !== name) : [...form.events, name]);
  const cls = (s) => (s === 'sent' ? 'ok' : s === 'suppressed' ? 'warn' : 'bad');
  const where = (ch) => (ch.kind === 'ntfy' ? `${ch.config.url} / ${ch.config.topic}` : ch.kind === 'webhook' ? ch.config.url : `${ch.config.to.join(', ')} via ${ch.config.host}`);
  const admin = $derived(auth.role === 'admin');
</script>

<div class="row head"><h1>Alerts</h1></div>
{#if !admin}<div class="card muted">This account can only read: changing alerts needs an admin account.</div>{/if}
{#if error}<p class="err">{error}</p>{/if}
{#if notice}<p style="color: var(--ok)">{notice}</p>{/if}

<fieldset disabled={!admin}>
  <div class="card">
    <div class="row" style="justify-content: space-between">
      <h2 style="margin:0">Channels</h2>
      <button class="primary" onclick={() => open(null)}>Add a channel</button>
    </div>
    <p class="muted small">
      A channel is one place where messages go. For your phone: install the <b>ntfy</b> app, subscribe to the same
      server and topic, and use a long random topic name (on a public ntfy server the topic is the only secret).
    </p>
    <table>
      <thead><tr><th>Name</th><th>Kind</th><th>To</th><th>Sends</th><th></th></tr></thead>
      <tbody>
        {#each channels as ch (ch.id)}
          <tr class:off={!ch.enabled}>
            <td>{ch.name}{#if !ch.enabled} <span class="badge">off</span>{/if}</td>
            <td>{ch.kind}</td><td class="muted">{where(ch)}</td>
            <td>{ch.events.join(', ')}</td>
            <td><div class="row actions">
              <button disabled={busy} onclick={() => test(ch)}>Test</button>
              <button disabled={busy} onclick={() => open(ch)}>Edit</button>
              <button disabled={busy} onclick={() => toggle(ch)}>{ch.enabled ? 'Turn off' : 'Turn on'}</button>
              <button class="danger" disabled={busy} onclick={() => remove(ch)}>Delete</button>
            </div></td>
          </tr>
        {:else}
          <tr><td colspan="5" class="muted">No channel yet: nobody is told anything.</td></tr>
        {/each}
      </tbody>
    </table>
  </div>

  {#if editing !== null}
    <form class="card" onsubmit={save}>
      <h2>{editing === 0 ? 'New channel' : 'Edit channel'}</h2>
      <div class="fields">
        <label>Name <input bind:value={form.name} required maxlength="64" /></label>
        {#if editing === 0}
          <label>Kind
            <select value={form.kind} onchange={(e) => (form = { ...blank(e.target.value), name: form.name, events: form.events })}>
              <option value="ntfy">ntfy (phone push)</option><option value="webhook">webhook (JSON)</option><option value="email">e-mail</option>
            </select>
          </label>
        {/if}
        {#if form.kind === 'ntfy'}
          <label>Server <input bind:value={form.cfg.url} required placeholder="https://ntfy.sh or http://pi:2586" /></label>
          <label>Topic <input bind:value={form.cfg.topic} required placeholder="a long random name" /></label>
          <label>Access token (optional) <input bind:value={form.cfg.token} type="password" placeholder={editing ? MASK : ''} /></label>
        {:else if form.kind === 'webhook'}
          <label>URL <input bind:value={form.cfg.url} required placeholder="https://..." /></label>
          <label>Secret (optional, signs the body: X-Signature) <input bind:value={form.cfg.secret} type="password" placeholder={editing ? MASK : ''} /></label>
        {:else}
          <label>SMTP server <input bind:value={form.cfg.host} required /></label>
          <label>Port <input bind:value={form.cfg.port} type="number" placeholder="587" /></label>
          <label>Security
            <select bind:value={form.cfg.security}><option value="starttls">STARTTLS</option><option value="ssl">SSL/TLS</option><option value="none">none</option></select>
          </label>
          <label>User <input bind:value={form.cfg.username} autocomplete="off" /></label>
          <label>Password <input bind:value={form.cfg.password} type="password" autocomplete="new-password" placeholder={editing ? MASK : ''} /></label>
          <label>From <input bind:value={form.cfg.from} required placeholder="monitor@example.com" /></label>
          <label>To (comma separated) <input bind:value={form.cfg.to} required /></label>
        {/if}
      </div>
      <h2 style="margin-top:1rem">Send a message when</h2>
      {#each events as e}
        <label class="check"><input type="checkbox" checked={form.events.includes(e.name)} onchange={() => flip(e.name)} /> {e.description}</label>
      {/each}
      <div class="row" style="margin-top:1rem">
        <button class="primary" disabled={busy || !form.events.length}>Save</button>
        <button type="button" onclick={() => (editing = null)}>Cancel</button>
      </div>
    </form>
  {/if}
</fieldset>

<div class="card">
  <h2>What was sent</h2>
  <table>
    <thead><tr><th>Time</th><th>Channel</th><th>Message</th><th>Result</th></tr></thead>
    <tbody>
      {#each log as r (r.id)}
        <tr>
          <td>{fmtTime(r.ts)}</td><td>{r.channel}</td><td>{r.title}</td>
          <td><span class="badge {cls(r.status)}">{r.status}</span> <span class="muted">{r.error ?? ''}</span></td>
        </tr>
      {:else}
        <tr><td colspan="4" class="muted">Nothing sent yet.</td></tr>
      {/each}
    </tbody>
  </table>
</div>

<style>
  fieldset { border: 0; padding: 0; margin: 0; display: flex; flex-direction: column; gap: .8rem; min-width: 0; }
  .head { margin-bottom: .4rem; }
  .small { font-size: .85rem; }
  .fields { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: .8rem; }
  label { display: flex; flex-direction: column; gap: .25rem; font-size: .9rem; }
  label.check { flex-direction: row; align-items: center; gap: .5rem; }
  tr.off td { opacity: .55; }
  .actions { flex-wrap: nowrap; }
</style>
