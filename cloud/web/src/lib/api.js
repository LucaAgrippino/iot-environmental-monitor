// Thin wrapper around the backend's HTTP API (see /docs on the backend for every endpoint).
async function call(path, opts = {}) {
  const r = await fetch('/api' + path, opts);
  if (!r.ok) {
    let detail = r.statusText;
    try { detail = (await r.json()).detail ?? detail; } catch {}
    throw new Error(typeof detail === 'string' ? detail : JSON.stringify(detail));
  }
  return r.status === 204 ? null : r.json();
}
export const get = (p) => call(p);
export const post = (p, body = {}) =>
  call(p, { method: 'POST', body: JSON.stringify(body), headers: { 'content-type': 'application/json' } });
export const put = (p, file) => call(p, { method: 'PUT', body: file });
export const del = (p) => call(p, { method: 'DELETE' });

export const fmtTime = (s) => (s ? new Date(s * 1000).toLocaleString() : '-');
export const ago = (s) => {
  const d = Math.max(0, Math.floor(Date.now() / 1000 - s));
  return d < 60 ? `${d} s ago` : d < 3600 ? `${Math.floor(d / 60)} min ago` : d < 86400 ? `${Math.floor(d / 3600)} h ago` : `${Math.floor(d / 86400)} d ago`;
};
