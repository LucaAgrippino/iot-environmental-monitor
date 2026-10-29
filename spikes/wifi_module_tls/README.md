# Spike: the ISM43362's own TLS (branch `spike/wifi-module-tls`, never merged)

**Question.** The Gateway runs mbedTLS on the host and reserves a 44 KB static arena for it
(`tls_arena`). The WiFi module has its own TLS (`P1=3` TCP-SSL, certificates by `PF`/`PG`,
verification by `P9`). Does that work with our broker, and with AWS IoT Core? If yes, most of the
arena (GM-O1 in `docs/lld/application/gateway-main.md`) is not needed.

**What this is.** A bring-up image that sends raw AT commands (`wifi_spike_raw_at`, compiled in only
with `-DWIFI_SPIKE_RAW_AT`) and logs every reply with CR/LF visible. It builds with `build.sh`, so
**no `.cproject` change** is needed and the IDE build is untouched. It does not use mbedTLS, so no arena.

## Run it against the local Mosquitto broker

```bash
cd spikes/wifi_module_tls
# 1. config from the existing test certificates (the files stay in the gitignored certs/ folder)
./gen_spike_config.sh ../../firmware/gateway/certs/ca.crt ../../firmware/gateway/certs/client.crt \
                      ../../firmware/gateway/certs/client.key 192.168.0.218 8883 2
./build.sh
# 2. broker running with TLS (scripts/start-mosquitto-tls.sh); watch its log in another terminal:
#    docker logs -f bringup-mosquitto      (or: mosquitto_sub -h ... --cafile ... -t 'spike/#')
# 3. flash and read the UART (recipe: see the hardware bench notes)
openocd -s <st_scripts> -f ../../firmware/gateway/gateway.cfg \
        -c "program build/spike.elf verify reset exit"
stty -F /dev/ttyACM0 115200 raw; cat /dev/ttyACM0 | tee spike-run.log
```

## Run it against AWS IoT Core

```bash
./gen_spike_config.sh AmazonRootCA1.pem device-cert.pem.crt private.pem.key \
                      <prefix>-ats.iot.<region>.amazonaws.com 8883 2 1,2
./build.sh
```

`1,2` is the Inventek sheet's AWS selection (`PF=1,2`; cert set 2). The host is a DNS name, so the
image sends `D0=<name>` first (the module resolves it). Use an `-ats` endpoint with the Amazon Root CA 1
certificate and a Thing policy that allows `iot:Connect` and `iot:Publish` on `spike/wifi-tls`.
Pass `-` for the three PEM files to reuse what the module already stores.

## How to read the result

| Log line | Meaning |
|---|---|
| `S0 I?` | module firmware string. The driver requires `C3.5.2.3`; record what it prints |
| `S1 PG ...` all `err=0` | the module accepted the PEM files (an RSA PKCS#8 key may be refused: note the reply) |
| `S3 P6=1 ... RESULT: TLS handshake ACCEPTED` | **the module's TLS works with this broker and these certificates** |
| `S3 ... REFUSED` | read the reply and the broker log: "no shared cipher" or "no suitable signature algorithm" means a cipher or signature limit of the module (the Spotflow write-up reports ECDSA-only signature algorithms and no SHA-384) |
| `S4 R0` shows `<20><02><00><00>` | MQTT CONNACK accepted: application data flows both ways through the module's TLS |
| broker log shows the `spike/wifi-tls` publish | the full path works |

Also record: handshake time (`S3 ... ms`), and whether `SNI` matters for AWS (a refusal against AWS
with a working local run points at SNI or the chain, not at the client certificate).

## Cautions

* `PG` **writes the module's flash** (cert set 0 for the local run, set 2 for AWS). It does not touch
  the firmware, and the application's mbedTLS path does not read those sets.
* `certs/spike_config.h` holds the private key; it is generated into the gitignored `certs/` folder.
  The image logs replies only, never the PEM data it sends.
* The key and certificate sizes matter: the spike's command buffer is 2048 bytes (the product driver's is 1536).
