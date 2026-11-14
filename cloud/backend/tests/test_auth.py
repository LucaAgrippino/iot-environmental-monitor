from dataclasses import replace

import pytest
from fastapi.testclient import TestClient

from iotcloud import admin, auth, ingest
from iotcloud.api import create_app

DEV = "dev-1"
PW = "correct horse battery"


@pytest.fixture(autouse=True)
def clean_fails():
    auth._fails.clear()


@pytest.fixture()
def app_client(settings, db):
    import time
    ingest.handle(db, "iotmonitor", f"dt/iotmonitor/{DEV}/health", b"{}", int(time.time()))  # heard just now
    auth.add_user(db, "luca", PW, "admin")
    auth.add_user(db, "guest", PW, "viewer")
    sent = []
    c = TestClient(create_app(replace(settings, auth_required=True), db, start_mqtt=False,
                              publisher=lambda t, p, q: sent.append(t)))
    return c, sent, db


def login(c, user="luca", pw=PW):
    return c.post("/api/auth/login", json={"username": user, "password": pw})


def test_password_hash_is_salted_and_checked():
    a, b = auth.hash_password(PW), auth.hash_password(PW)
    assert a != b and auth.verify_password(PW, a) and not auth.verify_password(PW + "x", a)
    assert not auth.verify_password(PW, "garbage")
    with pytest.raises(auth.AuthError):
        auth.hash_password("short")


def test_everything_needs_a_login_except_health_and_the_login_itself(app_client):
    c, _, _ = app_client
    assert c.get("/api/healthz").status_code == 200
    for path in ("/api/devices", f"/api/devices/{DEV}/telemetry", "/api/images", "/api/auth/me", "/api/stream"):
        assert c.get(path).status_code == 401, path
    assert c.post(f"/api/devices/{DEV}/control", json={"command": "read"}).status_code == 401


def test_login_sets_a_strict_httponly_cookie_and_opens_the_api(app_client):
    c, _, _ = app_client
    r = login(c)
    assert r.status_code == 200 and r.json() == {"user": "luca", "role": "admin"}
    cookie = r.headers["set-cookie"].lower()
    assert "httponly" in cookie and "samesite=strict" in cookie and "session=" in cookie
    assert c.get("/api/devices").status_code == 200
    assert c.get("/api/auth/me").json()["user"] == "luca"
    assert c.post("/api/auth/logout").status_code == 200
    assert c.get("/api/devices").status_code == 401


def test_wrong_credentials_are_refused_the_same_way(app_client):
    c, _, _ = app_client
    a, b = login(c, pw="wrong password!"), login(c, user="nobody")
    assert a.status_code == b.status_code == 401 and a.json() == b.json()


def test_viewer_reads_but_cannot_change_anything(app_client):
    c, sent, _ = app_client
    login(c, "guest")
    assert c.get("/api/devices").status_code == 200
    assert c.post(f"/api/devices/{DEV}/control", json={"command": "restart"}).status_code == 403
    assert c.put("/api/images/x.ota", content=b"x").status_code == 403
    assert c.post("/api/auth/logout").status_code == 200  # but may log out
    assert sent == []


def test_cross_site_post_is_refused_even_with_a_valid_cookie(app_client):
    c, sent, _ = app_client
    login(c)
    r = c.post(f"/api/devices/{DEV}/control", json={"command": "read"}, headers={"origin": "http://evil.example"})
    assert r.status_code == 403 and sent == []
    ok = c.post(f"/api/devices/{DEV}/control", json={"command": "read"}, headers={"origin": "http://testserver"})
    assert ok.status_code == 202


def test_lockout_after_repeated_failures(app_client):
    c, _, _ = app_client
    for _ in range(auth.MAX_FAILS):
        assert login(c, pw="wrong password!").status_code == 401
    assert login(c).status_code == 429  # even the right password waits


def test_new_password_ends_old_sessions_and_database_holds_no_token(app_client):
    c, _, db = app_client
    token = login(c).cookies.get("session") or c.cookies.get("session")
    assert token and token not in str(db.query("SELECT * FROM sessions"))
    auth.add_user(db, "luca", "another long password", "admin")
    assert c.get("/api/devices").status_code == 401


def test_admin_cli(tmp_path, monkeypatch, capsys):
    db = str(tmp_path / "u.sqlite3")
    monkeypatch.setenv("PW", PW)
    assert admin.main(["--db", db, "add-user", "ann", "--role", "viewer", "--password-env", "PW"]) == 0
    assert admin.main(["--db", db, "list"]) == 0 and "ann" in capsys.readouterr().out
    assert admin.main(["--db", db, "set-password", "ghost", "--password-env", "PW"]) == 1
    assert admin.main(["--db", db, "delete-user", "ann"]) == 0
