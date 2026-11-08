# LLD Companion: CertStore

**Version:** 1.1 · **Date:** 2026-11-08 · **Board:** Gateway · **Layer:** Middleware
**Satisfies:** REQ-LI-040 (WiFi credentials), REQ-LI-050 (cloud endpoint, at boot), REQ-LI-060 (cloud certificates), REQ-NF-302 (no plaintext passphrase or private key)
**HLD anchor:** the `CertStore` partition of `docs/hld/flash-partition-layout.md` (QSPI `0x9001_0000`, 64 KB).

## 1. Responsibility

CertStore keeps what the console provisions and the firmware cannot compile in: the WiFi network name and
passphrase, the MQTT broker host name, the CA certificate, the client certificate and the client private key. It decrypts them into one RAM
copy that WifiTask (through `main.c`) and MqttClient read directly. When nothing is stored, `main.c` uses the
build-time bring-up values, so a board that was never provisioned still works.

## 2. Storage

Device offset `0x0001_0000` (the HLD partition). Two 8 KB slots (two erase sectors each), written alternately.

| Part | Size | Content |
|---|---|---|
| Header | 16 B | magic `CERT`, sequence, CRC-32 of the body as stored, version (2) |
| Body | 6 922 B | lengths, broker host name (128), SSID (64), passphrase (64, encrypted), CA (2 048), client certificate (2 048), private key (2 560, encrypted) |

A write goes to the slot that does not hold the copy in use: erase both sectors, program the body, program the
header last. A slot is valid only when magic, version and CRC match (the CRC is computed over the body read back in 256-byte pieces, so a candidate never needs its own 7 KB buffer); the valid slot with the higher sequence wins. A
power cut before the header is written leaves the previous slot intact (tested). `cert_store_init()` is
tolerant: an empty or damaged store reads as "nothing provisioned".

The limits fit RSA-4096: a PKCS#8 private key is 1 218 bytes for RSA-2048, 1 794 for RSA-3072 and 2 375 for
RSA-4096 (limit 2 560); an RSA-4096 certificate is about 1.4 KB (limit 2 048). An earlier layout assumed an EC key
of 512 bytes and could not hold the bench RSA-2048 key; it was found on the board (CST-O1).

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
`cert_store_endpoint()`, `cert_store_set_endpoint(host)` (letters, digits, dots and hyphens, up to 127), `cert_store_item(CA|CLIENT|KEY, &len)`, `cert_store_identity_complete()`, `cert_store_set_wifi(ssid, pass)`,
`cert_store_set_item(item, der, len)` (the data must be a DER SEQUENCE of exactly that length),
`cert_store_clear()`, `cert_store_sequence()`. The flash is injected (`iqspi_flash_t`), as in ConfigStore.

Memory: the RAM copy (6.9 KB) is placed in SRAM2 (`.bss.sram2`), because the 96 KB bank is nearly full. A failed write restores the RAM copy by reloading the newest valid
slot, not from a stack copy: **the first version kept a 3.7 KB backup on the console task's stack and hung the
console on the first confirmation** (found on the board).

## 5. Use

- Console: `prov set wifi-ssid`, `prov set wifi-pass` (staged; both are needed; `prov commit` stores them),
  `prov set mqtt-endpoint` (stored here and in the configuration), `prov cert add <ca|client|key> <base64>` in
  chunks of at most 96 characters, `prov cert end <item>`, `prov cert reset` (drop a half-built item), `prov cert clear`,
  `prov cert status`. `scripts/provision_device.py` sends a PEM set (converted to DER) and the
  credentials.
- The broker endpoint is read from CertStore by `main.c` (`broker_name()`), for the name lookup and for the
  certificate check; it applies after the next restart. It is kept here, and not read from the stored configuration, because the boot
  task runs before LifecycleController loads the configuration, and the main stack cannot take `config_store_load` (below).
- WiFi credentials apply at the next connection attempt. The certificates and the key apply after the next restart
  (the MQTT configuration is built once at boot).

## 6. Tests

`tests/gateway/middleware/cert_store/test_cert_store.c`, 23 cases: empty store, null arguments, persistence across
init, secrets not in plaintext in the flash, exact read-back, other unique ID, completeness, size limits (a 1 218-byte
key fits), format check, input validation, slot alternation, fallback to the older slot, a power cut before the
header, a flash failure, clear, a fresh keystream per write. Console cases: `test_console_service_gw_tui.c`
(chunk assembly, error paths, staging, apply).

**Hardware (2026-11-08, Gateway):** a wrong stored passphrase makes the board stay offline ("no access point after
3 attempts") although the build-time one is correct, which proves the stored values are the ones used; the correct
credentials and the bench RSA-2048 identity, stored from the console, survive a reset, the board joins the network
and connects to the broker over mutual TLS (52 messages published, 0 failed).

## 7. Findings from the board

- The key limit, the stack backup and the chunk acknowledgement of the first versions (above).
- **The main stack is about 1 KB.** The 96 KB bank is 98.9 % full, so what is left for the stack in `main()` is
  what the linker leaves above `.bss`. `cert_store_init()` fits (a 256-byte buffer); `config_store_load()` did not, and calling it
  there or at the start of the boot task made the Gateway reset in a loop. Anything new that runs in `main()` needs a stack check.
- **A spinning WiFi wait starved the watchdog (a defect of the Gateway, not of CertStore).** With the broker
  unreachable, the WiFi driver polled the module with 100 microsecond busy delays for the whole 10 to 19 s of a failing
  connect, which kept the priority-1 lifecycle task from kicking the watchdog: the Gateway reset every 10 to 20 s. REQ-NF-200
  (keep working when the cloud is unreachable) was not met. `prv_wait_drdy` now spins for 2 ms and then sleeps one
  tick per poll through `wifi_driver_wait_1ms()` (`main.c` overrides the weak default). Checked on the board: with a wrong
  endpoint the uptime keeps counting and the Gateway publishes again as soon as the endpoint is right.

## 8. Read-out protection (CST-O3)

The key is derived from the chip's unique ID. The external QSPI flash is not covered by the MCU's read-out protection, so
what protects the two secrets is that the ID is not available to someone who only has the QSPI contents. With the debug
port open (RDP level 0) a probe reads the ID and the derivation is reproducible. At **RDP level 1** the probe can no longer
read the internal flash, the RAM or the ID, and cannot load code to read it; that is the production setting.
`scripts/gateway-bootloader-wrp.sh rdp1` (needs `CONFIRM_RDP1=yes`) sets it; `status` reports the level. Going back to level 0 erases the
internal flash (bootloader and application are flashed again), and the QSPI contents survive. Level 2 is permanent and is never used.
The procedure is part of the final flash, after `set` of the bootloader write protection (BL-O4). It is not set on the bench board.

## 9. Open items

| ID | Item | Status |
|---|---|---|
| CST-O1 | Key and certificate sizes | **Closed 2026-11-08:** limits 2 560 and 2 048 bytes (RSA-4096), slots of 8 KB, streaming validation |
| CST-O2 | Passphrase in the console scrollback | **Closed 2026-11-08:** the echo is masked |
| CST-O3 | Key bound to the chip ID only | **Closed 2026-11-08:** threat model and the RDP level 1 step (section 8), guarded script step; level 1 is set at the final flash |
| CST-O4 | The endpoint used for the connection and the certificate check was the build-time one | **Closed 2026-11-08:** stored in CertStore, used by `main.c`; verified on the board in both directions |
| CST-O5 | The MQTT port is still 8883 (the configuration has `mqtt_port`, it is not used by the connection) | Open, low |
