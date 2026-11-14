# Security review

**Date:** 2026-11-14. **Scope:** the whole system as deployed: Field Device and Gateway firmware, the OTA update path, the broker and its PKI, the cloud service and its
deployment on the Raspberry Pi, the repository and its CI. **Context:** a personal project on a home network; the repository is public.
**Result:** no critical or high finding. Two medium findings (device secrets at rest, Pi hardening), the rest low or informational. Nothing has been changed by this review.

## 1. Method and limits

- **Done:** a code review of the paths that handle untrusted input or secrets (broker configuration and access list, PKI tool, Gateway TLS client, command and OTA chunk handling,
  update verification, credential storage, cloud login, API and uploads, web app, service and proxy configuration, CI); a scan of the repository and its history for secrets;
  non-destructive probes of the live cloud (unauthenticated requests, headers, cookie flags); read-only checks on the Pi (ports, permissions, SSH, firewall, updates).
- **Not done:** fuzzing of the MQTT, JSON or Modbus parsers; a cipher-suite scan of the broker; a scan of the dependencies for known vulnerabilities (no scanner was run: versions are
  in section 5); a review of all firmware for memory safety (only the network-input paths were read); any physical attack.

## 2. What is done well

- **Broker:** mutual TLS 1.2 with a private CA, no anonymous access, certificate revocation list, one TLS listener. The access list gives each device only its own topics, keyed by the
  certificate's common name and not by the client id, which a client chooses itself (`broker/mosquitto/acl.conf`). The cloud service and the OTA tool have separate, narrow rules.
- **Gateway TLS client:** the server certificate is required, checked against our own CA only, and the host name is verified (`mqtt_client.c:591-606`).
- **Firmware updates:** ECDSA P-256 over a header that includes the body's SHA-256, so version, load address and body are bound together; the public key is a fail-closed placeholder in the
  repository; a tampered image and an image signed with another key were refused on the real Gateway (validation V-29).
- **Network input:** command and topic copies are clamped to fixed buffers; every OTA chunk must match the job, offset and length the Gateway asked for.
- **Cloud login:** scrypt password hashes, constant-time comparison, a dummy hash for unknown users, random session tokens stored only as SHA-256, `HttpOnly; Secure; SameSite=strict` cookie,
  lockout after repeated failures, cross-site request check. Every API route I probed answered 401 without a login. Strong headers (CSP with `frame-ancestors 'none'`, HSTS, nosniff).
- **Deployment:** the service runs as its own user with `NoNewPrivileges`, a read-only system, no home access and restricted address families; the backend listens on localhost only; the
  secrets of the service are readable by root and the service account only. SQL is parameterised (the one built fragment comes from a fixed list).
- **Repository:** no private keys in the current tree or in any pushed commit outside `tests/` and `vendor/` (the only matches in the whole history are in the local stash, S-8); the test keys under `tests/` are throw-away signing vectors, different from the release key; no credential literals in the current tree.

## 3. Findings

| ID | Severity | Finding | Evidence | Recommendation |
|---|---|---|---|---|
| S-1 | Medium | **The Gateway's private key and Wi-Fi passphrase are obfuscated, not protected.** They are XORed with a keystream derived from `SHA-256(public salt || chip unique ID)`; the unique ID is the device serial, which is in every topic and in the cloud data, and the chip's read-out protection is off. Anyone with the board and a debug cable can recover both. The code says so itself. REQ-NF-302 ("not plaintext") is met in letter only. | `firmware/gateway/middleware/cert_store/cert_store.c:70-103`; measured on the Gateway: option register `0xffeff8aa`, RDP level 0 (0xAA) | Accept for a home device, or set read-out protection level 1 (reversible by a mass erase, which also erases the key) and accept the harder flashing; the certificate can be revoked (`iotpki.py`) if the board is lost. |
| S-2 | Medium | **The Pi is lightly hardened.** SSH accepts passwords (and X11 forwarding is on), the firewall is inactive, 42 packages have security updates pending. Ports 22, 80, 443 and 8883 are open to the whole LAN. | `sshd -T`, `ufw status`, `apt list --upgradable` on the Pi | Key-only SSH, `ufw` allowing only 22, 80, 443 and 8883 from the LAN, `apt upgrade` and unattended security updates. |
| S-3 | Low | **The API documentation is public.** `/docs`, `/redoc` and `/openapi.json` answer 200 without a login and list every endpoint and command body. FastAPI adds them outside the login guard. | `cloud/backend/iotcloud/api.py:87` (`FastAPI(...)` without `docs_url=None`) | Pass `docs_url=None, redoc_url=None, openapi_url=None`. |
| S-4 | Low | **No downgrade protection.** An old, validly signed image can be installed again. A documented non-goal; it needs control of the OTA channel and an old image. | `docs/lld/middleware/firmware-store.md:87` | If wanted, refuse a version not newer than the running one in `verify` (one comparison). |
| S-5 | Low | **The release signing key is an unencrypted file** (mode 0600). | `~/keys/ota_release.pem` | Protect it with a passphrase; accepted for a personal project. |
| S-6 | Low | **Uploads have no size limit at the proxy.** The whole body is read into memory before the 2 MB check; only an authenticated admin can reach it. | `api.py:257`, `ota.py:60`, `deploy/Caddyfile` | `request_body { max_size 3MB }` in Caddy. |
| S-7 | Low | **Login lockout is per user name, in memory.** Anyone on the LAN can keep the admin account locked out; there is no per-address limit. | `auth.py:22,75-78` | Add a per-address limit; accept otherwise. |
| S-8 | Low | **A local git stash holds old bring-up secrets**: the private keys of the old bench CA and of its server, client and operator certificates, the old bring-up client key, `spike_config.h` (a private key) and `bringup_secrets.h` (the Wi-Fi credentials). It exists only in the local repository and was never pushed; the old bench CA is not the production CA, so its keys do not open your broker. | `git stash list` (`stash@{0}`, commit `b97d571`) | `git stash drop` once you are sure you do not need it. |
| S-9 | Low | **A stale bench key remains on the Pi** (`iotcloud.key` of the old bench CA), not reachable by other users because of the parent directory's permissions, but of no use. | `/etc/iotcloud/certs-bench/` | Delete the folder. |
| S-10 | Low | **The CI has no supply-chain hardening beyond read-only permissions.** Actions are pinned by tag, not commit; one third-party action; no Dependabot; no security policy; `main` has no branch protection. | `.github/workflows/*.yml`; the repository settings | Dependabot for Actions and pip, branch protection requiring the CI checks, a short `SECURITY.md`. |
| S-11 | Info | The notification webhook accepts any `http(s)` address, including internal ones (admin-configured). | `cloud/backend/iotcloud/notify.py:99` | Block private ranges if the cloud is ever exposed beyond the LAN. |
| S-12 | Info | Device certificates are valid 10 years and there is no rotation routine; revocation is manual. | `broker/iotpki.py:42` | Fine here; note it. |
| S-13 | Info | The broker does not restrict cipher suites (TLS 1.2 only, library defaults). | `broker/mosquitto/mosquitto.conf` | Restrict to ECDHE with AES-GCM if the Gateway's mbedTLS offers them (check first). |
| S-14 | Info | Dependencies were not scanned for known vulnerabilities. | section 5 | Run `pip-audit` for the Python pins; check the mbedTLS and coreMQTT release notes. |

## 4. Where the real risk is

The Pi listens on the LAN only (the backend is on localhost behind the proxy); whether your router forwards any port to it is outside what I could check, so confirm that none is. The risks that matter are therefore someone **on the LAN** and someone **with the
boards in hand**. For the first, S-2 and S-3 are the cheap, worthwhile fixes. For the second, S-1 is the only thing that gives away a secret, and a lost board can be answered by revoking its certificate.
Everything an attacker could do over the network to a device needs a certificate from your CA or control of the cloud service, and the update path does not trust the channel (it trusts the signature).

## 5. Versions noted

mbedTLS 3.6.7, coreMQTT v2.3.1, FreeRTOS kernel at commit `18ed888`, LVGL at commit `74d0a81`; Python: FastAPI 0.162.2, Starlette 1.10.0, uvicorn 0.54.0, paho-mqtt 2.2.0, pydantic 2.14.3
(`cloud/deploy/requirements.lock`).
