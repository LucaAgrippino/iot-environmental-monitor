"""Checks of tools/ota/make_image.py key loading (security review S-5). Run: python3 -m pytest tools/ota/test_make_image.py"""
import os
import sys
from pathlib import Path

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

sys.path.insert(0, str(Path(__file__).parent))
import make_image  # noqa: E402


def write_key(path, passphrase=None):
    key = ec.generate_private_key(ec.SECP256R1())
    enc = serialization.BestAvailableEncryption(passphrase.encode()) if passphrase else serialization.NoEncryption()
    path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, enc))
    return key


def test_a_plain_key_loads_as_before(tmp_path):
    k = write_key(tmp_path / "k.pem")
    assert make_image.raw_public_key(make_image.load_private_key(tmp_path / "k.pem")) == make_image.raw_public_key(k)


def test_an_encrypted_key_loads_with_the_passphrase_from_the_environment(tmp_path, monkeypatch):
    k = write_key(tmp_path / "k.pem", "correct horse battery")
    monkeypatch.setenv("OTA_KEY_PASSPHRASE", "correct horse battery")
    assert make_image.raw_public_key(make_image.load_private_key(tmp_path / "k.pem")) == make_image.raw_public_key(k)


def test_an_encrypted_key_without_a_passphrase_is_refused_when_there_is_no_terminal(tmp_path, monkeypatch):
    write_key(tmp_path / "k.pem", "correct horse battery")
    monkeypatch.delenv("OTA_KEY_PASSPHRASE", raising=False)
    monkeypatch.setattr(sys.stdin, "isatty", lambda: False)
    with pytest.raises(SystemExit) as e:
        make_image.load_private_key(tmp_path / "k.pem")
    assert "encrypted" in str(e.value)


def test_a_wrong_passphrase_is_refused(tmp_path, monkeypatch):
    write_key(tmp_path / "k.pem", "correct horse battery")
    monkeypatch.setenv("OTA_KEY_PASSPHRASE", "wrong one")
    with pytest.raises(ValueError):
        make_image.load_private_key(tmp_path / "k.pem")
