<script>
  import { login } from './lib/auth.svelte.js';
  let username = $state('');
  let password = $state('');
  let error = $state('');
  let busy = $state(false);

  async function submit(ev) {
    ev.preventDefault();
    busy = true;
    error = '';
    try {
      await login(username, password);
    } catch (e) {
      error = e.message;
    } finally {
      busy = false;
    }
  }
</script>

<div class="wrap">
  <form class="card" onsubmit={submit}>
    <h1>Environmental Monitor</h1>
    <p class="muted">Sign in to see the devices.</p>
    <label>User <input bind:value={username} autocomplete="username" required /></label>
    <label>Password <input bind:value={password} type="password" autocomplete="current-password" required /></label>
    {#if error}<p class="err">{error}</p>{/if}
    <button class="primary" disabled={busy}>Sign in</button>
  </form>
</div>

<style>
  .wrap { min-height: 100vh; display: grid; place-items: center; padding: 16px; }
  form { width: min(360px, 100%); display: flex; flex-direction: column; gap: .8rem; }
  label { display: flex; flex-direction: column; gap: .25rem; font-size: .9rem; }
  p { margin: 0; }
</style>
