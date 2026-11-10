"""Robot library: Gateway over-the-air update cases, driving scripts/ota_server.py (the operator and file server).

Images are made from the release binaries of an already signed release (slot A and slot B bodies, ~/rel/<version>/gateway_slot_?.bin),
re-signed with a chosen version and key, so no firmware build is needed. The run goes against the bench broker (anonymous clients allowed,
bench CA) and is meant for the PC with the Gateway reachable through the Raspberry Pi broker.
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
REL = os.environ.get("VERIFY_REL", os.path.expanduser("~/rel"))
KEY = os.environ.get("VERIFY_OTA_KEY", os.path.expanduser("~/keys/ota_release.pem"))
HOST = os.environ.get("VERIFY_OTA_HOST", "192.168.0.209")
CERTS = os.path.join(ROOT, "firmware/gateway/certs")


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
               "--cafile", os.path.join(CERTS, "ca.crt"), "--cert", os.path.join(CERTS, "client.crt"),
               "--key", os.path.join(CERTS, "client.key"), "--no-hostname-check", "--timeout", str(int(timeout) - 30)]
        cmd += extra.split()
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=int(timeout))
        self.out = r.stdout + r.stderr
        m = re.findall(r"\b(success|failed|rolled_back|rejected)\b", self.out.split("result")[-1] if "result" in self.out else self.out)
        if not m:
            raise AssertionError("no terminal OTA report in the output:\n" + self.out[-1200:])
        return m[-1]

    def last_ota_output(self):
        return self.out
