"""Users, passwords and sessions (the AWS counterpart: Cognito user pool + IAM roles).

- Two roles: "admin" may do everything, "viewer" may only read (GET). A command, a config change, an upload or an
  OTA is a POST/PUT/DELETE, so the role check is one rule on the HTTP method.
- Passwords are stored as scrypt hashes (standard library; ~16 MB of RAM for 100 ms, fine on a Pi 3).
- A session is a random token in an HttpOnly, SameSite=Strict cookie. The database keeps only its SHA-256, so a
  copy of the database does not give a working session.
- Users are managed on the machine with `python -m iotcloud.admin`, not over HTTP: nobody can create an account
  through the API, and there is no "forgot password" to attack.
"""
import hashlib
import hmac
import secrets
import time

ROLES = ("admin", "viewer")
SESSION_TTL_S = 7 * 24 * 3600
MIN_PASSWORD = 10
SCRYPT = {"n": 2**14, "r": 8, "p": 1}

# A failed login costs the next attempt more and more time (per user name, in memory).
MAX_FAILS, LOCK_S, WINDOW_S = 5, 60, 300
_fails: dict[str, list[float]] = {}


class AuthError(ValueError):
    pass


def hash_password(password: str, salt: bytes | None = None) -> str:
    if len(password) < MIN_PASSWORD:
        raise AuthError(f"the password needs at least {MIN_PASSWORD} characters")
    salt = salt or secrets.token_bytes(16)
    h = hashlib.scrypt(password.encode(), salt=salt, **SCRYPT)
    return f"scrypt${SCRYPT['n']}${SCRYPT['r']}${SCRYPT['p']}${salt.hex()}${h.hex()}"


def verify_password(password: str, stored: str) -> bool:
    try:
        algo, n, r, p, salt, digest = stored.split("$")
        if algo != "scrypt":
            return False
        h = hashlib.scrypt(password.encode(), salt=bytes.fromhex(salt), n=int(n), r=int(r), p=int(p))
        return hmac.compare_digest(h, bytes.fromhex(digest))
    except (ValueError, TypeError):
        return False


def _digest(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()


def add_user(db, name: str, password: str, role: str, now: int | None = None):
    if role not in ROLES:
        raise AuthError(f"role must be one of {', '.join(ROLES)}")
    if not name or len(name) > 64 or not name.isprintable():
        raise AuthError("bad user name")
    db.execute("INSERT INTO users(name, pw_hash, role, created) VALUES(?,?,?,?) "
               "ON CONFLICT(name) DO UPDATE SET pw_hash=excluded.pw_hash, role=excluded.role",
               (name, hash_password(password), role, int(time.time()) if now is None else now))
    db.execute("DELETE FROM sessions WHERE user=?", (name,))  # a new password ends the old sessions


def delete_user(db, name: str) -> bool:
    existed = bool(db.query("SELECT 1 FROM users WHERE name=?", (name,)))
    db.execute("DELETE FROM sessions WHERE user=?", (name,))
    db.execute("DELETE FROM users WHERE name=?", (name,))
    return existed


def list_users(db):
    return db.query("SELECT name, role, created FROM users ORDER BY name")


def _locked(name: str, now: float) -> bool:
    recent = [t for t in _fails.get(name, []) if now - t < WINDOW_S]
    _fails[name] = recent
    return len(recent) >= MAX_FAILS and now - recent[-1] < LOCK_S


def login(db, name: str, password: str, now: float | None = None):
    """Returns (token, role). Raises AuthError. The same message for an unknown user and a wrong password."""
    now = time.time() if now is None else now
    if _locked(name, now):
        raise AuthError("too many attempts, wait a minute")
    rows = db.query("SELECT pw_hash, role FROM users WHERE name=?", (name,))
    # verify even for an unknown user, so the response time does not tell which names exist
    ok = verify_password(password, rows[0]["pw_hash"] if rows else "scrypt$16384$8$1$00$00")
    if not rows or not ok:
        _fails.setdefault(name, []).append(now)
        raise AuthError("wrong user name or password")
    _fails.pop(name, None)
    token = secrets.token_urlsafe(32)
    db.execute("INSERT INTO sessions(token_hash, user, created, expires) VALUES(?,?,?,?)",
               (_digest(token), name, int(now), int(now) + SESSION_TTL_S))
    db.execute("DELETE FROM sessions WHERE expires<?", (int(now),))
    return token, rows[0]["role"]


def user_for(db, token: str | None, now: float | None = None):
    """{"name", "role"} for a valid session token, else None."""
    if not token:
        return None
    now = int(time.time() if now is None else now)
    rows = db.query("SELECT u.name, u.role FROM sessions s JOIN users u ON u.name=s.user "
                    "WHERE s.token_hash=? AND s.expires>?", (_digest(token), now))
    return rows[0] if rows else None


def logout(db, token: str | None):
    if token:
        db.execute("DELETE FROM sessions WHERE token_hash=?", (_digest(token),))
