// One server-sent-events connection for the whole app. `live.n` changes on every event: components watch it
// to refresh, `live.last` is the latest event (device, kind, ...).
export const live = $state({ connected: false, n: 0, last: null });

let es = null;

export function startLive() {
  es?.close(); // never two streams, e.g. after signing out and in again
  es = new EventSource('/api/stream');
  es.onopen = () => (live.connected = true);
  es.onerror = () => (live.connected = false); // the browser reconnects by itself
  es.onmessage = (e) => {
    live.last = JSON.parse(e.data);
    live.n++;
  };
}

export function stopLive() {
  es?.close();
  es = null;
  live.connected = false;
}
