"""The broker, for real: Mosquitto in Docker with our configuration and certificates from iotpki.py.
Skipped when Docker or the image is missing. Each test tries to break one rule."""
import os
import re
import shutil
import ssl
import subprocess
import threading
import time
import uuid
from pathlib import Path

import paho.mqtt.client as mqtt
import pytest

from iotpki import Pki

BROKER = Path(__file__).resolve().parents[1] / "mosquitto"
IMAGE = "eclipse-mosquitto:2"
PORT = 18883
A, B = "002d004f3442501020323848", "0033001f3442501020323849"

pytestmark = pytest.mark.skipif(
    shutil.which("docker") is None or subprocess.run(["docker", "image", "inspect", IMAGE], capture_output=True).returncode != 0,
    reason=f"needs docker and the {IMAGE} image")


class Peer:
    """A client with a certificate; collects what it receives."""
    def __init__(self, files: Path, name: str, clean=True, cn_dir=None):
        self.name, self.got, self.connected = name, [], threading.Event()
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=name, clean_session=clean)
        d = files
        self.c.tls_set(ca_certs=str(d / "ca.crt"), certfile=str(d / f"{name}.crt"), keyfile=str(d / f"{name}.key"),
                       tls_version=ssl.PROTOCOL_TLSv1_2)
        self.c.on_connect = lambda c, u, f, rc, p=None: self.connected.set() if rc == 0 else None
        self.c.on_message = lambda c, u, m: self.got.append((m.topic, m.payload))

    def open(self, timeout=4) -> bool:
        try:
            self.c.connect("localhost", PORT)
        except OSError:
            return False
        self.c.loop_start()
        ok = self.connected.wait(timeout)
        if not ok:
            self.close()
        return ok

    def close(self):
        self.c.loop_stop()
        self.c.disconnect()

    def topics(self):
        return [t for t, _ in self.got]


def settle(pred, timeout=3.0):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.05)
    return False


@pytest.fixture(scope="module")
def env(tmp_path_factory):
    root = tmp_path_factory.mktemp("iot")
    pki = Pki(root / "pki")
    pki.init()
    pki.server(["DNS:localhost", "IP:127.0.0.1"])
    pki.service("iotcloud")
    pki.service("ota-operator")
    pki.device(A)
    pki.device(B)
    other = Pki(root / "rogue")  # a CA that is not ours
    other.init()
    other.device(A)
    conf = root / "iot"
    pki.export_broker(conf)
    text = (BROKER / "mosquitto.conf").read_text().replace("/etc/mosquitto/iot", "/mosquitto/iot")
    text = text.replace("/var/lib/mosquitto/", "/mosquitto/data/").replace("listener 8883 0.0.0.0", "listener 8883 0.0.0.0")
    (conf / "mosquitto.conf").write_text(text)
    shutil.copyfile(BROKER / "acl.conf", conf / "acl.conf")
    data = root / "data"
    data.mkdir()
    name = f"iotpki-test-{uuid.uuid4().hex[:8]}"
    # run as the current user: the key file is 0600 and Mosquitto refuses a key it cannot read
    subprocess.run(["docker", "run", "-d", "--rm", "--name", name, "-u", f"{os.getuid()}:{os.getgid()}",
                    "-p", f"127.0.0.1:{PORT}:8883", "-v", f"{conf}:/mosquitto/iot:ro", "-v", f"{data}:/mosquitto/data",
                    IMAGE, "mosquitto", "-c", "/mosquitto/iot/mosquitto.conf"], check=True, capture_output=True)
    ready = False
    for _ in range(40):
        time.sleep(0.25)
        probe = Peer(root / "pki" / "issued" / "iotcloud", "iotcloud")
        if probe.open(1):
            probe.close()
            ready = True
            break
    if not ready:
        logs = subprocess.run(["docker", "logs", name], capture_output=True, text=True)
        subprocess.run(["docker", "rm", "-f", name], capture_output=True)
        pytest.fail("broker did not start:\n" + logs.stdout + logs.stderr)
    yield {"pki": pki, "issued": root / "pki" / "issued", "rogue": root / "rogue" / "issued", "conf": conf, "name": name}
    subprocess.run(["docker", "rm", "-f", name], capture_output=True)


def peer(env, who, **kw):
    return Peer(env["issued"] / who, who, **kw)


def test_no_certificate_and_a_foreign_certificate_are_refused(env):
    anon = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="anon")
    anon.tls_set(ca_certs=str(env["issued"] / "iotcloud" / "ca.crt"), tls_version=ssl.PROTOCOL_TLSv1_2)
    with pytest.raises((OSError, ssl.SSLError)):
        anon.connect("localhost", PORT)
        anon.loop_start()
        time.sleep(1.5)
        raise OSError("not refused" if anon.is_connected() else "refused")
    anon.loop_stop()
    rogue = Peer(env["rogue"] / A, A)
    assert rogue.open() is False


def test_a_device_reaches_the_cloud_only_under_its_own_name(env):
    cloud, dev = peer(env, "iotcloud"), peer(env, A)
    assert cloud.open() and dev.open()
    cloud.c.subscribe("dt/iotmonitor/+/#", 1)
    time.sleep(0.4)
    dev.c.publish(f"dt/iotmonitor/{B}/telemetry", b"forged", 1)       # another device's topic: dropped
    dev.c.publish(f"dt/iotmonitor/{A}/telemetry", b"mine", 1)         # its own: delivered
    assert settle(lambda: (f"dt/iotmonitor/{A}/telemetry", b"mine") in cloud.got)
    assert f"dt/iotmonitor/{B}/telemetry" not in cloud.topics()
    cloud.close(); dev.close()


def test_a_device_cannot_send_commands_or_listen_to_others(env):
    cloud, a, b = peer(env, "iotcloud"), peer(env, A), peer(env, B)
    assert cloud.open() and a.open() and b.open()
    a.c.subscribe("cmd/iotmonitor/#", 1)
    b.c.subscribe("cmd/iotmonitor/#", 1)
    cloud.c.subscribe("cmd/iotmonitor/#", 1)  # the cloud's own subscription must stay silent too (it only writes)
    time.sleep(0.4)
    b.c.publish(f"cmd/iotmonitor/{A}/control", b'{"command":"restart"}', 1)  # a device impersonating the cloud
    cloud.c.publish(f"cmd/iotmonitor/{A}/control", b'{"command":"read"}', 1)
    cloud.c.publish(f"cmd/iotmonitor/{B}/control", b'{"command":"read"}', 1)
    assert settle(lambda: len(a.got) >= 1 and len(b.got) >= 1)
    time.sleep(0.5)
    assert a.got == [(f"cmd/iotmonitor/{A}/control", b'{"command":"read"}')]   # only its own, only from the cloud
    assert b.got == [(f"cmd/iotmonitor/{B}/control", b'{"command":"read"}')]
    for p in (cloud, a, b):
        p.close()


def test_the_ota_tool_may_do_the_ota_conversation_and_nothing_else(env):
    op, cloud, dev = peer(env, "ota-operator"), peer(env, "iotcloud"), peer(env, A)
    assert op.open() and cloud.open() and dev.open()
    dev.c.subscribe("cmd/iotmonitor/#", 1)
    cloud.c.subscribe("dt/iotmonitor/#", 1)
    op.c.subscribe("dt/iotmonitor/#", 1)
    time.sleep(0.4)
    op.c.publish(f"cmd/iotmonitor/{A}/config", b"{}", 1)             # not its business
    op.c.publish(f"cmd/iotmonitor/{A}/ota", b'{"job":1}', 1)         # its business
    dev.c.publish(f"dt/iotmonitor/{A}/ota/result", b"done", 1)
    dev.c.publish(f"dt/iotmonitor/{A}/telemetry", b"t", 1)
    assert settle(lambda: len(dev.got) >= 1 and len(op.got) >= 1)
    time.sleep(0.5)
    assert dev.topics() == [f"cmd/iotmonitor/{A}/ota"]
    assert op.topics() == [f"dt/iotmonitor/{A}/ota/result"]          # sees OTA results, not telemetry
    for p in (op, cloud, dev):
        p.close()


def test_a_message_sent_while_the_cloud_was_away_is_delivered_when_it_returns(env):
    cloud, dev = peer(env, "iotcloud", clean=False), peer(env, A)
    assert cloud.open() and dev.open()
    cloud.c.subscribe("dt/iotmonitor/+/alarms", 1)
    time.sleep(0.4)
    cloud.close()                                                     # the backend restarts
    dev.c.publish(f"dt/iotmonitor/{A}/alarms", b"HIGH", 1).wait_for_publish(3)
    back = peer(env, "iotcloud", clean=False)
    assert back.open()
    assert settle(lambda: (f"dt/iotmonitor/{A}/alarms", b"HIGH") in back.got)
    back.close(); dev.close()


def test_an_oversized_message_is_refused(env):
    dev, cloud = peer(env, A), peer(env, "iotcloud")
    assert dev.open() and cloud.open()
    cloud.c.subscribe("dt/iotmonitor/#", 1)
    time.sleep(0.4)
    dev.c.publish(f"dt/iotmonitor/{A}/telemetry", b"x" * 70000, 0)
    time.sleep(1.0)
    assert cloud.got == []
    dev.close(); cloud.close()


def test_a_revoked_device_is_refused_after_the_list_is_reloaded(env):
    ok = peer(env, B)
    assert peer(env, A).open() and ok.open()
    ok.close()
    env["pki"].revoke(A)
    shutil.copyfile(env["pki"].ca_dir / "crl.pem", env["conf"] / "crl.pem")
    subprocess.run(["docker", "kill", "-s", "HUP", env["name"]], check=True, capture_output=True)  # = systemctl reload
    time.sleep(1.0)
    assert peer(env, A).open() is False
    assert peer(env, B).open() is True  # nobody else is disturbed
