#!/usr/bin/env python3
"""iotpki - the certificate authority of the IoT Environmental Monitor (the AWS counterpart: IoT Core certificates
and AWS Private CA).

One CA signs three kinds of certificate. The common name (CN) is the identity the broker's access list uses:

    device    CN = the device serial (24 hex characters, the CPU unique id)     -> provisioned from the console
    service   CN = iotcloud | ota-operator                                       -> the cloud backend, the OTA tool
    server    the broker itself (names and IPs in the subject alternative name)

    iotpki.py init                                   # once: creates the CA
    iotpki.py server --san IP:192.168.0.209 --san DNS:envmon.lan
    iotpki.py service iotcloud
    iotpki.py device 002d004f3442501020323848        # then: scripts/provision_device.py --ca ... --cert ... --key ...
    iotpki.py revoke 002d004f3442501020323848        # a lost or stolen device: the broker refuses it from then on
    iotpki.py list | status
    iotpki.py export-broker --out DIR                # what the broker machine needs (never the CA key)

Everything lives in $IOTPKI_DIR (default ~/.iot-pki), outside the repository. The CA key is the one secret that
matters: keep it on this machine, back it up, and never copy it to the Pi. To protect it with a passphrase, set
IOTPKI_CA_PASSPHRASE when you run the commands.

Key sizes follow what the Gateway can store and verify: RSA 2048 everywhere (CertStore slots are 2 KB per
certificate, 2.5 KB for the key), TLS 1.2, SHA-256.
"""
import argparse
import datetime as dt
import ipaddress
import json
import os
import re
import shutil
import sys
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

KEY_BITS = 2048
CA_DAYS, SERVER_DAYS, DEVICE_DAYS, SERVICE_DAYS, CRL_DAYS = 3650, 1095, 3650, 730, 1825
SERIAL_RE = re.compile(r"^[0-9a-f]{24}$")
SERVICES = ("iotcloud", "ota-operator")
SIM_RE = re.compile(r"^sim-[0-9a-z]{1,16}$")  # simulated devices, for development only


class PkiError(Exception):
    pass


def now() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0)


class Pki:
    def __init__(self, root: Path):
        self.root = Path(root)
        self.ca_dir = self.root / "ca"
        self.inv_path = self.ca_dir / "inventory.json"

    # -- storage -----------------------------------------------------------------------------------------------------
    def _passphrase(self):
        p = os.environ.get("IOTPKI_CA_PASSPHRASE")
        return p.encode() if p else None

    def _write_key(self, path: Path, key, passphrase=None):
        enc = serialization.BestAvailableEncryption(passphrase) if passphrase else serialization.NoEncryption()
        path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)  # born private: no moment of 0644
        with os.fdopen(fd, "wb") as f:
            f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, enc))

    def _write_cert(self, path: Path, cert):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))

    def _inv(self) -> dict:
        if not self.inv_path.exists():
            raise PkiError(f"no CA in {self.root}: run 'iotpki.py init' first")
        return json.loads(self.inv_path.read_text())

    def _save_inv(self, inv: dict):
        self.inv_path.write_text(json.dumps(inv, indent=2, sort_keys=True))

    def _ca(self):
        cert = x509.load_pem_x509_certificate((self.ca_dir / "ca.crt").read_bytes())
        key = serialization.load_pem_private_key((self.ca_dir / "ca.key").read_bytes(), password=self._passphrase())
        return cert, key

    # -- operations --------------------------------------------------------------------------------------------------
    def init(self, name="IoT Environmental Monitor CA"):
        if self.inv_path.exists():
            raise PkiError(f"a CA already exists in {self.root}")
        key = rsa.generate_private_key(65537, KEY_BITS)
        subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
        t = now()
        cert = (x509.CertificateBuilder().subject_name(subject).issuer_name(subject).public_key(key.public_key())
                .serial_number(x509.random_serial_number()).not_valid_before(t - dt.timedelta(minutes=1))
                .not_valid_after(t + dt.timedelta(days=CA_DAYS))
                .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
                .add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False), critical=True)
                .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
                .sign(key, hashes.SHA256()))
        self._write_key(self.ca_dir / "ca.key", key, self._passphrase())
        self._write_cert(self.ca_dir / "ca.crt", cert)
        self._save_inv({"crl_number": 0, "certs": {}})
        self._write_crl()
        return cert

    def _issue(self, name: str, kind: str, days: int, usage, san=None):
        inv = self._inv()
        old = inv["certs"].get(name)
        if old and not old["revoked"]:
            raise PkiError(f"{name} already has a certificate; revoke it first (iotpki.py revoke {name})")
        ca_cert, ca_key = self._ca()
        key = rsa.generate_private_key(65537, KEY_BITS)
        t = now()
        b = (x509.CertificateBuilder().subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)]))
             .issuer_name(ca_cert.subject).public_key(key.public_key()).serial_number(x509.random_serial_number())
             .not_valid_before(t - dt.timedelta(minutes=1)).not_valid_after(t + dt.timedelta(days=days))
             .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
             .add_extension(x509.KeyUsage(True, False, True, False, False, False, False, False, False), critical=True)
             .add_extension(x509.ExtendedKeyUsage([usage]), critical=False)
             .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
             .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()), critical=False))
        if san:
            b = b.add_extension(x509.SubjectAlternativeName(san), critical=False)
        cert = b.sign(ca_key, hashes.SHA256())
        out = self.root / "issued" / name
        self._write_key(out / f"{name}.key", key)
        self._write_cert(out / f"{name}.crt", cert)
        shutil.copyfile(self.ca_dir / "ca.crt", out / "ca.crt")
        inv["certs"][name] = {
            "kind": kind, "serial": format(cert.serial_number, "x"), "not_after": cert.not_valid_after_utc.isoformat(),
            "revoked": None, "sha256": cert.fingerprint(hashes.SHA256()).hex()}
        self._save_inv(inv)
        return out

    def device(self, serial: str, allow_sim=False):
        if not (SERIAL_RE.match(serial) or (allow_sim and SIM_RE.match(serial))):
            raise PkiError("a device name is the serial: 24 lowercase hex characters (the CPU unique id)")
        return self._issue(serial, "device", DEVICE_DAYS, ExtendedKeyUsageOID.CLIENT_AUTH)

    def service(self, name: str):
        if name not in SERVICES:
            raise PkiError(f"service must be one of {', '.join(SERVICES)}")
        return self._issue(name, "service", SERVICE_DAYS, ExtendedKeyUsageOID.CLIENT_AUTH)

    def server(self, sans: list[str], name="iot-broker"):
        names = []
        for s in sans or ["DNS:localhost", "IP:127.0.0.1"]:
            kind, _, value = s.partition(":")
            if kind == "IP":
                names.append(x509.IPAddress(ipaddress.ip_address(value)))
            elif kind == "DNS" and value:
                names.append(x509.DNSName(value))
            else:
                raise PkiError(f"bad name '{s}': use IP:1.2.3.4 or DNS:host.name")
        return self._issue(name, "server", SERVER_DAYS, ExtendedKeyUsageOID.SERVER_AUTH, names)

    def revoke(self, name: str):
        inv = self._inv()
        c = inv["certs"].get(name)
        if not c or c["revoked"]:
            raise PkiError(f"{name}: no active certificate")
        c["revoked"] = now().isoformat()
        self._save_inv(inv)
        self._write_crl()

    def _write_crl(self):
        inv = self._inv()
        ca_cert, ca_key = self._ca()
        inv["crl_number"] += 1
        t = now()
        b = (x509.CertificateRevocationListBuilder().issuer_name(ca_cert.subject)
             .last_update(t - dt.timedelta(minutes=1)).next_update(t + dt.timedelta(days=CRL_DAYS))
             .add_extension(x509.CRLNumber(inv["crl_number"]), critical=False)
             .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()), critical=False))
        for c in inv["certs"].values():
            if c["revoked"]:
                b = b.add_revoked_certificate(x509.RevokedCertificateBuilder().serial_number(int(c["serial"], 16))
                                              .revocation_date(dt.datetime.fromisoformat(c["revoked"])).build())
        crl = b.sign(ca_key, hashes.SHA256())
        (self.ca_dir / "crl.pem").write_bytes(crl.public_bytes(serialization.Encoding.PEM))
        self._save_inv(inv)

    def export_broker(self, out: Path):
        """The files a broker machine needs: CA certificate, its own certificate and key, the revocation list."""
        server_dir = self.root / "issued" / "iot-broker"
        if not server_dir.exists():
            raise PkiError("no server certificate yet: iotpki.py server --san ...")
        out.mkdir(parents=True, exist_ok=True)
        files = {"ca.crt": self.ca_dir / "ca.crt", "crl.pem": self.ca_dir / "crl.pem",
                 "server.crt": server_dir / "iot-broker.crt", "server.key": server_dir / "iot-broker.key"}
        for dest, src in files.items():
            shutil.copyfile(src, out / dest)
        os.chmod(out / "server.key", 0o600)
        return out

    def rows(self):
        inv = self._inv()
        t = now()
        rows = []
        for name, c in sorted(inv["certs"].items()):
            left = (dt.datetime.fromisoformat(c["not_after"]) - t).days
            status = "revoked" if c["revoked"] else ("EXPIRED" if left < 0 else "active")
            rows.append({"name": name, "kind": c["kind"], "status": status, "days_left": left, "serial": c["serial"]})
        return rows

    def status(self):
        """Days until the CA certificate and the revocation list expire. An expired list makes the broker refuse everyone."""
        ca_cert, _ = self._ca()
        crl = x509.load_pem_x509_crl((self.ca_dir / "crl.pem").read_bytes())
        t = now()
        return {"ca_days_left": (ca_cert.not_valid_after_utc - t).days, "crl_days_left": (crl.next_update_utc - t).days,
                "revoked": len(list(crl))}

    def refresh_crl(self):
        self._write_crl()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=os.environ.get("IOTPKI_DIR", str(Path.home() / ".iot-pki")))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("init")
    s = sub.add_parser("server")
    s.add_argument("--san", action="append", help="IP:1.2.3.4 or DNS:name (repeat); default localhost and 127.0.0.1")
    d = sub.add_parser("device")
    d.add_argument("serial")
    d.add_argument("--allow-sim", action="store_true", help="accept a name like sim-0001 (the simulator)")
    sv = sub.add_parser("service")
    sv.add_argument("name", choices=SERVICES)
    r = sub.add_parser("revoke")
    r.add_argument("name")
    sub.add_parser("list")
    sub.add_parser("status")
    sub.add_parser("refresh-crl")
    e = sub.add_parser("export-broker")
    e.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    pki = Pki(Path(a.dir))
    try:
        if a.cmd == "init":
            pki.init()
            print(f"CA created in {pki.ca_dir}. Keep ca.key here and back it up.")
        elif a.cmd == "server":
            print(f"broker certificate in {pki.server(a.san)}")
        elif a.cmd == "device":
            out = pki.device(a.serial, a.allow_sim)
            print(f"device certificate in {out}\nprovision it:\n  scripts/provision_device.py --port /dev/ttyACMx "
                  f"--ca {out}/ca.crt --cert {out}/{a.serial}.crt --key {out}/{a.serial}.key")
        elif a.cmd == "service":
            print(f"service certificate in {pki.service(a.name)}")
        elif a.cmd == "revoke":
            pki.revoke(a.name)
            print(f"{a.name} revoked; copy crl.pem to the broker (export-broker) and reload it")
        elif a.cmd == "list":
            for r_ in pki.rows():
                print(f"{r_['name']:28} {r_['kind']:8} {r_['status']:8} {r_['days_left']:5} days  serial {r_['serial'][:12]}")
        elif a.cmd == "status":
            st = pki.status()
            print(f"CA certificate: {st['ca_days_left']} days left; revocation list: {st['crl_days_left']} days left "
                  f"({st['revoked']} revoked)")
        elif a.cmd == "refresh-crl":
            pki.refresh_crl()
            print("revocation list renewed")
        elif a.cmd == "export-broker":
            print(f"broker files in {pki.export_broker(Path(a.out))}")
    except PkiError as ex:
        print(f"error: {ex}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
