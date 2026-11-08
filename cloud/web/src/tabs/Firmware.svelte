<script>
  import { get, post, put, del, fmtTime } from '../lib/api.js';
  import { live } from '../lib/live.svelte.js';
  import { auth } from '../lib/auth.svelte.js';

  let { device } = $props();
  let images = $state([]);
  let jobs = $state([]);
  let error = $state('');
  let busy = $state(false);
  let imageA = $state('');
  let imageB = $state('');
  let files = $state();
  const base = $derived(`/devices/${encodeURIComponent(device.id)}`);

  async function load() {
    try {
      [images, jobs] = await Promise.all([get('/images'), get(`${base}/ota?limit=10`)]);
    } catch (e) {
      error = e.message;
    }
  }
  $effect(() => {
    live.n; // chunk requests and results both arrive as events: the progress bar follows them
    load();
  });

  async function run(fn) {
    busy = true;
    error = '';
    try {
      await fn();
      await load();
    } catch (e) {
      error = e.message;
    } finally {
      busy = false;
    }
  }
  const upload = () => run(async () => {
    for (const f of files) await put(`/images/${encodeURIComponent(f.name)}`, f);
    files = undefined;
  });
  const start = () => run(() => post(`${base}/ota`, { image_a: imageA, image_b: imageB }));
  const cancel = (id) => run(() => post(`/ota/${id}/cancel`));
  const remove = (name) => run(() => del(`/images/${encodeURIComponent(name)}`));

  const active = $derived(jobs.find((j) => j.state === 'sent' || j.state === 'running'));
  const pct = (j) => Math.min(100, Math.round((100 * j.sent_bytes) / j.size_b));
  const cls = (s) => (s === 'success' ? 'ok' : s === 'sent' || s === 'running' ? 'warn' : 'bad');
  const kb = (n) => (n / 1024).toFixed(1) + ' KiB';
</script>

{#if error}<p class="err">{error}</p>{/if}

{#if auth.role !== 'admin'}<div class="card muted">This account can only read: updating needs an admin account.</div>{/if}
<fieldset disabled={auth.role !== 'admin'}>
<div class="card">
  <h2>Update the firmware</h2>
  <p class="muted small">
    The Gateway has two flash slots, and an image is linked for one of them: build both
    (<code>tools/ota/make_image.py</code>), upload them, pick one for each slot. The device downloads its slot's image
    chunk by chunk, checks the signature and reboots into it.
  </p>
  <div class="row">
    <select bind:value={imageA}><option value="">Image for slot A</option>{#each images as i}<option>{i.name}</option>{/each}</select>
    <select bind:value={imageB}><option value="">Image for slot B</option>{#each images as i}<option>{i.name}</option>{/each}</select>
    <button class="primary" disabled={busy || !imageA || !imageB || active} onclick={start}>Start update</button>
  </div>
  {#if active}
    <div class="progress">
      <div class="bar"><div style="width:{pct(active)}%"></div></div>
      <span>job {active.id}: {active.state}, {pct(active)} % ({kb(active.sent_bytes)} of {kb(active.size_b)})</span>
      <button class="danger" disabled={busy} onclick={() => cancel(active.id)}>Cancel</button>
    </div>
  {/if}
</div>

<div class="card">
  <h2>Firmware images</h2>
  <div class="row">
    <input type="file" multiple bind:files accept=".ota,.bin" />
    <button disabled={busy || !files?.length} onclick={upload}>Upload</button>
  </div>
  <table>
    <thead><tr><th>Name</th><th class="num">Size</th><th>SHA-256</th><th>Uploaded</th><th></th></tr></thead>
    <tbody>
      {#each images as i (i.name)}
        <tr>
          <td>{i.name}</td><td class="num">{kb(i.size)}</td><td><code>{i.sha256.slice(0, 16)}…</code></td><td>{fmtTime(i.uploaded)}</td>
          <td><button class="danger" disabled={busy} onclick={() => remove(i.name)}>Delete</button></td>
        </tr>
      {:else}
        <tr><td colspan="5" class="muted">No image uploaded.</td></tr>
      {/each}
    </tbody>
  </table>
</div>
</fieldset>

<div class="card">
  <h2>Updates of this device</h2>
  <table>
    <thead><tr><th>Job</th><th>Started</th><th>Slot A / B image</th><th>State</th><th>Detail</th></tr></thead>
    <tbody>
      {#each jobs as j (j.id)}
        <tr>
          <td>{j.id}</td><td>{fmtTime(j.created)}</td><td>{j.image_a} / {j.image_b}</td>
          <td><span class="badge {cls(j.state)}">{j.state}</span></td>
          <td class="muted">{j.detail ? (JSON.parse(j.detail).reason ?? JSON.parse(j.detail).result) : ''}</td>
        </tr>
      {:else}
        <tr><td colspan="5" class="muted">No update yet.</td></tr>
      {/each}
    </tbody>
  </table>
</div>

<style>
  fieldset { border: 0; padding: 0; margin: 0; display: flex; flex-direction: column; gap: .8rem; min-width: 0; }
  .small { font-size: .85rem; margin-top: 0; }
  .progress { display: flex; align-items: center; gap: .8rem; margin-top: .8rem; flex-wrap: wrap; }
  .bar { flex: 1 1 200px; height: 10px; background: var(--line); border-radius: 99px; overflow: hidden; }
  .bar div { height: 100%; background: var(--accent); transition: width .3s; }
</style>
