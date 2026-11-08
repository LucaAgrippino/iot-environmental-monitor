<script>
  let { device } = $props();
  const kinds = {
    telemetry: 'Readings (temperature, humidity, pressure)',
    health: 'Health reports',
    alarms: 'Alarms',
    events: 'Command, configuration and OTA results',
  };
  let kind = $state('telemetry');
  let format = $state('csv');
  let days = $state(1);
  const url = $derived(
    `/api/devices/${encodeURIComponent(device.id)}/export?kind=${kind}&format=${format}` +
      (days > 0 ? `&since=${Math.floor(Date.now() / 1000 - days * 86400)}` : '')
  );
</script>

<div class="card">
  <h2>Download what the cloud holds</h2>
  <div class="row">
    <select bind:value={kind}>{#each Object.entries(kinds) as [k, label]}<option value={k}>{label}</option>{/each}</select>
    <select bind:value={days}>
      <option value={1}>last 24 hours</option><option value={7}>last 7 days</option>
      <option value={30}>last 30 days</option><option value={0}>everything</option>
    </select>
    <select bind:value={format}><option value="csv">CSV</option><option value="json">JSON</option></select>
    <a href={url} download><button class="primary">Download</button></a>
  </div>
  <p class="muted">
    These are the records the cloud stored from the device's messages. The firmware does not send its own log lines yet;
    they stay on its serial console.
  </p>
</div>
