import json
import os
import stat

import pytest
from cryptography import x509
from cryptography.hazmat.primitives import serialization
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

import iotpki
from iotpki import Pki, PkiError

SERIAL = "002d004f3442501020323848"
OTHER = "0033001f3442501020323849"


@pytest.fixture()
def pki(tmp_path):
    p = Pki(tmp_path / "pki")
    p.init()
    return p


def load(path):
    return x509.load_pem_x509_certificate(path.read_bytes())


def test_device_certificate_is_signed_by_the_ca_and_named_by_serial(pki):
    out = pki.device(SERIAL)
    cert, ca = load(out / f"{SERIAL}.crt"), load(pki.ca_dir / "ca.crt")
    cert.verify_directly_issued_by(ca)
    assert cert.subject.get_attributes_for_oid(NameOID.COMMON_NAME)[0].value == SERIAL
    assert cert.extensions.get_extension_for_class(x509.ExtendedKeyUsage).value == x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH])
    assert cert.extensions.get_extension_for_class(x509.BasicConstraints).value.ca is False
    assert cert.public_key().key_size == 2048
    assert (out / "ca.crt").read_bytes() == (pki.ca_dir / "ca.crt").read_bytes()  # the set a device needs, in one folder


def test_private_keys_are_not_readable_by_others(pki):
    out = pki.device(SERIAL)
    for key in (out / f"{SERIAL}.key", pki.ca_dir / "ca.key"):
        assert stat.S_IMODE(os.stat(key).st_mode) == 0o600


def test_server_certificate_carries_its_names(pki):
    out = pki.server(["IP:192.168.0.209", "DNS:envmon.lan"])
    cert = load(out / "iot-broker.crt")
    san = cert.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
    assert [str(i) for i in san.get_values_for_type(x509.IPAddress)] == ["192.168.0.209"]
    assert san.get_values_for_type(x509.DNSName) == ["envmon.lan"]
    assert cert.extensions.get_extension_for_class(x509.ExtendedKeyUsage).value == x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH])
    with pytest.raises(PkiError):
        pki.server(["192.168.0.1"])


def test_names_are_checked(pki):
    for bad in ("sim-0001", "ABCDEF", SERIAL.upper(), SERIAL + "0", "iotcloud"):
        with pytest.raises(PkiError):
            pki.device(bad)
    pki.device("sim-0001", allow_sim=True)
    with pytest.raises(PkiError):
        pki.service("somebody")


def test_one_active_certificate_per_name_until_revoked(pki):
    pki.device(SERIAL)
    with pytest.raises(PkiError):
        pki.device(SERIAL)
    pki.revoke(SERIAL)
    pki.device(SERIAL)  # a replacement after revoking the old one
    rows = {r["name"]: r for r in pki.rows()}
    assert rows[SERIAL]["status"] == "active"


def test_revocation_list_names_the_revoked_serial_only(pki):
    pki.device(SERIAL)
    pki.device(OTHER)
    inv = json.loads(pki.inv_path.read_text())
    revoked_serial = int(inv["certs"][SERIAL]["serial"], 16)
    pki.revoke(SERIAL)
    crl = x509.load_pem_x509_crl((pki.ca_dir / "crl.pem").read_bytes())
    assert [r.serial_number for r in crl] == [revoked_serial]
    assert crl.is_signature_valid(load(pki.ca_dir / "ca.crt").public_key())
    assert pki.status()["revoked"] == 1 and pki.status()["crl_days_left"] > 1000
    with pytest.raises(PkiError):
        pki.revoke(OTHER + "x")


def test_export_for_the_broker_never_contains_the_ca_key(pki, tmp_path):
    with pytest.raises(PkiError):
        pki.export_broker(tmp_path / "out")  # no server certificate yet
    pki.server([])
    out = pki.export_broker(tmp_path / "out")
    assert sorted(p.name for p in out.iterdir()) == ["ca.crt", "crl.pem", "server.crt", "server.key"]
    assert b"BEGIN" in (out / "ca.crt").read_bytes()
    ca_key = (pki.ca_dir / "ca.key").read_bytes()
    assert all(ca_key != p.read_bytes() for p in out.iterdir())


def test_ca_key_can_be_protected_with_a_passphrase(tmp_path, monkeypatch):
    monkeypatch.setenv("IOTPKI_CA_PASSPHRASE", "a secret phrase")
    p = Pki(tmp_path / "enc")
    p.init()
    assert b"ENCRYPTED" in (p.ca_dir / "ca.key").read_bytes()
    p.device(SERIAL)  # works with the passphrase
    monkeypatch.delenv("IOTPKI_CA_PASSPHRASE")
    with pytest.raises(TypeError):  # a key that needs a password, none given
        p.device(OTHER)


def test_no_ca_no_service_and_no_second_init(tmp_path, pki):
    with pytest.raises(PkiError):
        Pki(tmp_path / "nothing").device(SERIAL)
    with pytest.raises(PkiError):
        pki.init()


def test_command_line(tmp_path, capsys):
    d = str(tmp_path / "cli")
    assert iotpki.main(["--dir", d, "init"]) == 0
    assert iotpki.main(["--dir", d, "device", SERIAL]) == 0
    assert "provision_device.py" in capsys.readouterr().out
    assert iotpki.main(["--dir", d, "device", "nope"]) == 1
    assert iotpki.main(["--dir", d, "list"]) == 0 and SERIAL in capsys.readouterr().out
