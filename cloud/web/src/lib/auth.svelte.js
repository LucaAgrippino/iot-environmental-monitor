// Who is logged in. `user` is null when nobody is (the app then shows the login screen).
export const auth = $state({ user: null, role: null, checked: false });

export async function whoami() {
  try {
    const r = await fetch('/api/auth/me');
    if (r.ok) {
      const me = await r.json();
      auth.user = me.user;
      auth.role = me.role;
      auth.requiresLogin = me.auth;
    } else auth.user = null;
  } catch {
    auth.user = null;
  }
  auth.checked = true;
}

export async function login(username, password) {
  const r = await fetch('/api/auth/login', {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ username, password }),
  });
  const body = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(body.detail ?? r.statusText);
  auth.user = body.user;
  auth.role = body.role;
  auth.requiresLogin = true;
}

export async function logout() {
  await fetch('/api/auth/logout', { method: 'POST' });
  auth.user = null;
}

export const isAdmin = () => auth.role === 'admin';
