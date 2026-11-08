# broker/ : the system's own certificate authority and broker configuration

The self-hosted counterpart of **AWS IoT Core** (the broker, its policies, device certificates) and **AWS Private CA**.

```text
iotpki.py            the certificate authority: CA, broker, device and service certificates, revocation
mosquitto/           broker configuration and the access list (who may publish and read what)
install-broker.sh    puts them on the Raspberry Pi (and can roll back); build-bundle.sh packs the files
tests/               unit tests of the CA, and a real Mosquitto in Docker that tries to break the rules
```

## The model

- **Identity is the certificate.** Every client presents a certificate signed by our CA; its common name (CN) is the
  user name. A device's CN is its serial (24 hex characters, the CPU unique id), which is also its MQTT client id.
- **The access list says what a name may do** (`mosquitto/acl.conf`):

| Name | May publish | May read |
|---|---|---|
| a device (any other name) | `dt/iotmonitor/<its own CN>/#` | `cmd/iotmonitor/<its own CN>/#` |
| `iotcloud` (the backend) | `cmd/iotmonitor/+/#` | `dt/iotmonitor/+/#` |
| `ota-operator` (the OTA tool) | `cmd/iotmonitor/+/ota`, `.../ota/data` | `dt/iotmonitor/+/ota/request`, `.../ota/result` |

- **Why `%u` and not the client id.** The bench ACL used `%c` (the MQTT client id). A client chooses that itself, so a
  device could have used another device's id and read its commands. The CN is fixed by the CA, so `%u` cannot be forged.
- **Revocation.** `iotpki.py revoke <serial>` adds the certificate to a revocation list (CRL); the broker refuses that
  certificate at the handshake from then on. This is "deactivate certificate" of AWS IoT.
- **Sizes follow the Gateway.** RSA 2048 and SHA-256 everywhere, TLS 1.2 only: what CertStore can hold (2 KB per
  certificate, 2.5 KB for the key) and what the bench already proves on the board.

## Using it

Everything is kept in `~/.iot-pki` (or `$IOTPKI_DIR`), outside the repository. **`ca/ca.key` is the one secret that
matters:** keep it on this machine, back it up, never copy it to the Pi. Set `IOTPKI_CA_PASSPHRASE` to encrypt it.

```sh
pip install cryptography                       # once (the cloud venv has it)
broker/iotpki.py init                          # once: creates the CA
broker/iotpki.py server --san IP:192.168.0.209 --san DNS:envmon.lan --san DNS:localhost --san IP:127.0.0.1
broker/iotpki.py service iotcloud              # the backend's certificate
broker/iotpki.py service ota-operator          # for scripts/ota_server.py
broker/iotpki.py device 002d004f3442501020323848
broker/iotpki.py list                          # who has a certificate, and for how long
broker/iotpki.py status                        # days left of the CA and of the revocation list
```

**A device:** `iotpki.py device <serial>` prints the exact `scripts/provision_device.py` command. The files are
`ca.crt`, `<serial>.crt` and `<serial>.key` in `~/.iot-pki/issued/<serial>/`. The serial is shown by the console.

**The cloud backend:** copy `ca.crt`, `iotcloud.crt`, `iotcloud.key` from `issued/iotcloud/` to `/etc/iotcloud/certs/`
on the Pi and set the three `IOTCLOUD_MQTT_*` lines (`cloud/deploy/iotcloud.env.example`).

**A lost device:**

```sh
broker/iotpki.py revoke 002d004f3442501020323848
broker/build-bundle.sh                               # packs the new list
scp broker/out/broker-bundle.tar.gz luca@192.168.0.209:   # then on the Pi: tar xzf, sudo ./install-broker.sh --update-crl
```

## Putting it on the Pi

```sh
broker/build-bundle.sh && scp broker/out/broker-bundle.tar.gz luca@192.168.0.209:
# on the Pi:
tar xzf broker-bundle.tar.gz && cd broker-bundle && sudo ./install-broker.sh --replace-bench
sudo ./install-broker.sh --rollback        # if you need the bench configuration back
```

**Read this first.** `--replace-bench` swaps the bench broker's configuration (bench CA, anonymous allowed) for this one.
From then on the bench certificates stop working: the Gateway and every script that connects to that broker need
certificates from the new CA. Do it when no verification run is using the Pi, and have the new device certificates
ready. If Mosquitto does not start, the installer restores the bench configuration by itself.

## What was tested

- `tests/test_pki.py`: certificate contents (names, usages, key size, chain), key file permissions, name rules, one
  active certificate per name, revocation list, export without the CA key, passphrase.
- `tests/test_broker.py` (needs Docker): a real Mosquitto with our configuration. No certificate and a certificate
  from another CA are refused; a device can publish only under its own name; a device cannot send commands, and cannot
  read another device's commands; the OTA tool can do the OTA conversation and nothing else; a message sent while the
  backend was away is delivered when it returns; an oversized message is refused; a revoked device is refused after a
  reload while the others are not disturbed. With a deliberately open access list four of these fail, so they do check.
- `test-install-broker.sh`: the installer in a Debian 13 container with its real Mosquitto 2.0.21 (the Pi's version),
  replacing a stand-in bench configuration, device and cloud talking through it, then `--rollback`.

- `test-e2e.sh`: the real backend and the simulator through the secured broker with certificates from `iotpki.py`: the
  device is seen, a command comes back accepted, an OTA reaches success.

Run them: `env -u PYTHONPATH ~/.venvs/envmon-cloud/bin/python -m pytest broker`.

**Not tested:** the installer under systemd on the Pi, and a real Gateway connecting with a certificate from this CA
(the Gateway's TLS stack, its CertStore slots and `scripts/provision_device.py` are proven with the bench certificate,
not yet with one from `iotpki.py`; the format is the same, RSA 2048 PEM).

## Limits and chores

- The revocation list is valid for 5 years; an expired list makes the broker refuse **everyone**. `iotpki.py status`
  shows the days left; `iotpki.py refresh-crl` renews it.
- The broker certificate lasts 3 years, service certificates 2, device certificates 10 (they are burnt in by a
  console command, so renewing means touching the device). The CA lasts 10 years.
- A device's certificate cannot be revoked by the device; it is the operator's decision. There is no automatic
  "certificate seen on two addresses" detection.
- The bench broker configuration in `tools/ota/mosquitto/` and `local-test/` is left as it is: it belongs to the
  bring-up with the bench CA.
