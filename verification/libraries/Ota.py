"""Robot library: Gateway over-the-air update cases, driving scripts/ota_server.py (the operator and file server).

Images are made from the release binaries of an already signed release (slot A and slot B bodies, ~/rel/<version>/gateway_slot_?.bin),
re-signed with a chosen version and key, so no firmware build is needed. The run goes against the bench broker (anonymous clients allowed,
bench CA) and is meant for the PC with the Gateway reachable through the Raspberry Pi broker.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
REL = os.environ.get("VERIFY_REL", os.path.expanduser("~/rel"))
KEY = os.environ.get("VERIFY_OTA_KEY", os.path.expanduser("~/keys/ota_release.pem"))
HOST = os.environ.get("VERIFY_OTA_HOST", "192.168.0.209")
CERTS = os.path.join(ROOT, "firmware/gateway/certs")


def _certs():
    """(ca, certificate, key) of the operator client: the repository's Gateway certificates on the PC; with VERIFY_CERTS set
    (the Raspberry Pi keeps ca.crt, operator.crt and operator.key outside the repository) that directory."""
    d = os.environ.get("VERIFY_CERTS")
    if d:
        return os.path.join(d, "ca.crt"), os.path.join(d, "operator.crt"), os.path.join(d, "operator.key")
    return (os.path.join(CERTS, "ca.crt"), os.path.join(CERTS, "client.crt"), os.path.join(CERTS, "client.key"))


class Ota:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self):
        self.work = tempfile.mkdtemp(prefix="ota-verify-")

    def _sign(self, body, slot, version, key, out):
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools/ota/make_image.py"), "sign", "--body", body, "--slot", slot,
                            "--version", version, "--private", key, "--out", out], capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError("signing failed: " + r.stderr[-400:])

    def make_images(self, name, from_release, version, wrong_key=False):
        """Sign slot A and B bodies of release `from_release` as `version`; with wrong_key a throw-away key signs them.
        Returns (path of image A, path of image B)."""
        key = KEY
        if str(wrong_key).lower() in ("true", "1", "yes"):
            key = os.path.join(self.work, "wrong.pem")
            from cryptography.hazmat.primitives import serialization
            from cryptography.hazmat.primitives.asymmetric import ec
            k = ec.generate_private_key(ec.SECP256R1())
            open(key, "wb").write(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        out = []
        for slot in ("A", "B"):
            body = os.path.join(REL, from_release, "gateway_slot_%s.bin" % slot.lower())
            dst = os.path.join(self.work, "%s_%s.ota" % (name, slot.lower()))
            self._sign(body, slot, version, key, dst)
            out.append(dst)
        return out[0], out[1]

    def run_update(self, device_id, image_a, image_b, job, extra="", timeout=900):
        """Run ota_server.py for one update; returns the terminal report word (success, failed, rolled_back, rejected)
        and keeps the whole output (see `last_ota_output`)."""
        cmd = [sys.executable, os.path.join(ROOT, "scripts/ota_server.py"), "--host", HOST, "--device-id", device_id,
               "--image-a", image_a, "--image-b", image_b, "--job", str(job),
               "--cafile", _certs()[0], "--cert", _certs()[1],
               "--key", _certs()[2], "--no-hostname-check", "--timeout", str(int(timeout) - 30)]
        cmd += extra.split()
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=int(timeout))
        self.out = r.stdout + r.stderr
        from robot.api import logger
        logger.info("ota_server output:\n" + self.out)
        m = re.findall(r"\b(success|failed|rolled_back|rejected)\b", self.out.split("result")[-1] if "result" in self.out else self.out)
        if not m:
            raise AssertionError("no terminal OTA report in the output:\n" + self.out[-1200:])
        return m[-1]

    def run_update_with_broker_outage(self, device_id, image_a, image_b, job, outage_s=110, silent_at=65536, timeout=900):
        """Like run_update, but when the download reaches `silent_at` the bench broker on the Raspberry Pi is stopped for
        `outage_s` seconds (ssh, passwordless sudo) and started again. The link is down, so the Gateway pauses instead of
        spending its retries (REQ-DM-051/052) and must resume at the saved offset; the file server only reconnects.
        Returns the terminal report word; the broker is always started again."""
        cmd = [sys.executable, os.path.join(ROOT, "scripts/ota_server.py"), "--host", HOST, "--device-id", device_id,
               "--image-a", image_a, "--image-b", image_b, "--job", str(job),
               "--cafile", _certs()[0], "--cert", _certs()[1],
               "--key", _certs()[2], "--no-hostname-check", "--timeout", str(int(timeout) - 30),
               "--silent-at", str(silent_at), "--silent-seconds", "1"]
        ssh = ["ssh", "-o", "ConnectTimeout=10", f"luca@{HOST}"]
        log = os.path.join(self.work, f"job{job}.log")
        with open(log, "w") as fh:
            proc = subprocess.Popen(cmd, stdout=fh, stderr=subprocess.STDOUT, text=True)
            try:
                deadline = time.time() + 300
                while time.time() < deadline and proc.poll() is None:
                    if "going silent" in open(log).read():
                        break
                    time.sleep(0.2)
                else:
                    raise AssertionError("the download never reached the stall offset:\n" + open(log).read()[-800:])
                subprocess.run(ssh + ["sudo systemctl stop mosquitto"], check=True, capture_output=True, timeout=60)
                time.sleep(float(outage_s))
            finally:
                subprocess.run(ssh + ["sudo systemctl start mosquitto"], capture_output=True, timeout=60)
            try:
                proc.wait(timeout=int(timeout))
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        self.out = open(log).read()
        m = re.findall(r"\b(success|failed|rolled_back|rejected)\b", self.out.split("result")[-1] if "result" in self.out else self.out)
        if not m:
            raise AssertionError("no terminal OTA report in the output:\n" + self.out[-1200:])
        return m[-1]

    def settle_gateway(self, device_id, quiet_s=30, timeout=420):
        """Wait until the Gateway is online (a telemetry message arrives) and no OTA result has been published for `quiet_s`
        seconds. The Gateway publishes its OTA results itself, late (after the reboot and reconnect), and a result carries
        no job id, so a result left over from one case would otherwise be taken for the answer of the next one.
        Returns the list of results seen while settling (they belong to earlier cases, and are logged)."""
        import paho.mqtt.client as mqtt
        topics = {"tele": f"dt/iotmonitor/{device_id}/telemetry", "res": f"dt/iotmonitor/{device_id}/ota/result"}
        seen, state = [], {"tele": False, "last_res": time.time()}

        def on_message(c, u, msg):
            if msg.topic == topics["res"]:
                seen.append(msg.payload.decode("latin-1"))
                state["last_res"] = time.time()
            else:
                state["tele"] = True

        try:
            c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="ota-settle")
        except AttributeError:
            c = mqtt.Client(client_id="ota-settle")
        import ssl
        c.tls_set(ca_certs=_certs()[0], certfile=_certs()[1],
                  keyfile=_certs()[2], cert_reqs=ssl.CERT_REQUIRED)
        c.tls_insecure_set(True)
        c.on_message = on_message
        c.on_connect = lambda cl, u, f, rc, *a: cl.subscribe([(topics["tele"], 0), (topics["res"], 1)])
        c.connect(HOST, 8883, 30)
        c.loop_start()
        try:
            deadline = time.time() + float(timeout)
            while time.time() < deadline:
                if state["tele"] and time.time() - state["last_res"] >= float(quiet_s):
                    return seen
                time.sleep(0.5)
            raise AssertionError("the Gateway did not settle (online and no OTA result for %ss); results seen: %s" % (quiet_s, seen))
        finally:
            c.loop_stop()
            c.disconnect()

    def last_ota_output(self):
        return self.out

    # ---- closure cases: malformed images, the reported reason, an update running in the background ------------------

    def make_malformed_images(self, name, from_release, version, mode):
        """Signed images of `from_release` spoiled in one way (the signature is not re-made: the Gateway has to refuse them
        before it gets to the cryptography, REQ-DM-060). mode: magic (the header magic is zero), header_size (the declared
        header size is wrong), oversize (600 KB of padding: larger than a slot). Returns (path of image A, path of image B)."""
        a, b = self.make_images(name, from_release, version)
        for path in (a, b):
            data = bytearray(open(path, "rb").read())
            if mode == "magic":
                data[0:4] = b"\x00\x00\x00\x00"
            elif mode == "header_size":
                struct.pack_into("<H", data, 6, 64)
            elif mode == "oversize":
                data += bytes(600 * 1024)
            else:
                raise AssertionError("unknown mode %r" % mode)
            open(path, "wb").write(bytes(data))
        return a, b

    def last_ota_reason(self):
        """The "reason" of the last terminal report the server printed (an empty string when there was none)."""
        found = re.findall(r'"result":"(?:failed|rejected|rolled_back)"[^}]*"reason":"(\w+)"', self.out)
        return found[-1] if found else ""

    def start_update(self, device_id, image_a, image_b, job, extra="", timeout=300):
        """Start ota_server.py in the background (for a second command while this update runs); see `finish_update`."""
        cmd = [sys.executable, os.path.join(ROOT, "scripts/ota_server.py"), "--host", HOST, "--device-id", device_id,
               "--image-a", image_a, "--image-b", image_b, "--job", str(job),
               "--cafile", _certs()[0], "--cert", _certs()[1],
               "--key", _certs()[2], "--no-hostname-check", "--timeout", str(int(timeout) - 30)]
        cmd += extra.split()
        self.bg = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    def finish_update(self, timeout=300):
        """Wait for the background update to end; returns its whole output."""
        try:
            out, _ = self.bg.communicate(timeout=int(timeout))
        except subprocess.TimeoutExpired:
            self.bg.kill()
            out, _ = self.bg.communicate()
        self.bg_out = out
        from robot.api import logger
        logger.info("background ota_server output:\n" + out)
        return out
