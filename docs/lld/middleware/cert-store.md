# LLD Companion: CertStore

**Version:** 1.0 · **Date:** 2026-11-08 · **Board:** Gateway · **Layer:** Middleware
**Satisfies:** REQ-LI-040 (WiFi credentials), REQ-LI-060 (cloud certificates), REQ-NF-302 (no plaintext passphrase or private key)
**HLD anchor:** the `CertStore` partition of `docs/hld/flash-partition-layout.md` (QSPI `0x9001_0000`, 64 KB).

## 1. Responsibility

CertStore keeps what the console provisions and the firmware cannot compile in: the WiFi network name and
passphrase, the CA certificate, the client certificate and the client private key. It decrypts them into one RAM
copy that WifiTask (through `main.c`) and MqttClient read directly. When nothing is stored, `main.c` uses the
build-time bring-up values, so a board that was never provisioned still works.

## 2. Storage

Device offset `0x0001_0000` (the HLD partition). Two 8 KB slots (two erase sectors each), written alternately.

| Part | Size | Content |
|---|---|---|
| Header | 16 B | magic `CERT`, sequence, CRC-32 of the body as stored, version |
| Body | 4 472 B | lengths, SSID (64), passphrase (64, encrypted), CA (1 536), client certificate (1 536), private key (1 280, encrypted) |

A write goes to the slot that does not hold the copy in use: erase both sectors, program the body, program the
header last. A slot is valid only when magic, version and CRC match; the valid slot with the higher sequence wins. A
power cut before the header is written leaves the previous slot intact (tested). `cert_store_init()` is
tolerant: an empty or damaged store reads as "nothing provisioned".

The private key limit is 1 280 bytes: **RSA-2048 PKCS#8 is 1 218**, which the bench key is. RSA-3072 and
RSA-4096 keys do not fit; EC keys do, with room to spare. This was found on the board, an earlier layout
assumed an EC key of 512 bytes.

## 3. Encryption (REQ-NF-302)

The passphrase and the private key are XORed with a keystream of `SHA-256(key || sequence || field || counter)` in 32-byte blocks,
with `key = SHA-256("envmon-cert-store-v1" || 96-bit MCU unique ID)`. The sequence changes with every write, so a
keystream is never used twice. SHA-256 is the mbedTLS one already in the image; the host tests use the same code.
The certificates are public and stored as they are.

**What this protects:** reading the flash with a probe or from another chip's dump does not reveal the two
secrets (the tests check that neither the passphrase nor the key appears in the flash bytes, and that another
unique ID reads garbage). **What it does not protect:** code running on this chip can derive the key. The
firmware readout protection (RDP level 1) is the answer to a local attacker and is outside this module.

## 4. Interface (`cert_store.h`)

`cert_store_init(flash, uid)`, `cert_store_has_wifi()`, `cert_store_ssid()`, `cert_store_password()`,
`cert_store_item(CA|CLIENT|KEY, &len)`, `cert_store_identity_complete()`, `cert_store_set_wifi(ssid, pass)`,
`cert_store_set_item(item, der, len)` (the data must be a DER SEQUENCE of exactly that length),
`cert_store_clear()`, `cert_store_sequence()`. The flash is injected (`iqspi_flash_t`), as in ConfigStore.

Memory: the RAM copy (4.5 KB) and one candidate buffer used while loading (4.5 KB) are placed in SRAM2
(`.bss.sram2`), because the 96 KB bank is nearly full. A failed write restores the RAM copy by reloading the newest valid
slot, not from a stack copy: **the first version kept a 3.7 KB backup on the console task's stack and hung the
console on the first confirmation** (found on the board).

## 5. Use

- Console: `prov set wifi-ssid`, `prov set wifi-pass` (staged; both are needed; `prov commit` stores them),
  `prov cert add <ca|client|key> <base64>` in chunks of at most 96 characters, `prov cert end <item>`,
  `prov cert clear`, `prov cert status`. `scripts/provision_device.py` sends a PEM set (converted to DER) and the
  credentials.
- WiFi credentials apply at the next connection attempt. The certificates and the key apply after the next restart
  (the MQTT configuration is built once at boot).

## 6. Tests

`tests/gateway/middleware/cert_store/test_cert_store.c`, 20 cases: empty store, null arguments, persistence across
init, secrets not in plaintext in the flash, exact read-back, other unique ID, completeness, size limits (a 1 218-byte
key fits), format check, input validation, slot alternation, fallback to the older slot, a power cut before the
header, a flash failure, clear, a fresh keystream per write. Console cases: `test_console_service_gw_tui.c`
(chunk assembly, error paths, staging, apply).

**Hardware (2026-11-08, Gateway):** a wrong stored passphrase makes the board stay offline ("no access point after
3 attempts") although the build-time one is correct, which proves the stored values are the ones used; the correct
credentials and the bench RSA-2048 identity, stored from the console, survive a reset, the board joins the network
and connects to the broker over mutual TLS (52 messages published, 0 failed).

## 7. Open items

| ID | Item |
|---|---|
| CST-O1 | RSA keys above 2048 bits do not fit (1 280 bytes); raise the slot or store the key in its own sector if needed |
| CST-O2 | The console echoes what is typed, so the passphrase appears in the log panel's scrollback; mask the value of `wifi-pass` in `repl` |
| CST-O3 | The key is derived from the unique ID alone; read-out protection level 1 and a per-device secret in the option bytes would raise the bar |
| CST-O4 | The MQTT endpoint name used for the certificate check (`tls_server_name`) is still the build-time one; only the address comes from the configuration |
