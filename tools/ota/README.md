# tools/ota

Build, sign and test OTA firmware images for the Gateway `FirmwareStore`.
Format and rationale: `docs/lld/middleware/firmware-store.md` §6, §11.

## Two images per release

The firmware slots are execute-in-place and not position-independent, so each
release is linked twice, once for Slot A (`0x0800_6000`) and once for Slot B
(`0x0808_0000`), and each is signed separately. `make_image.py sign` refuses a
body whose reset vector does not point inside the slot it is signed for.

```
make_image.py sign --body gateway_slot_b.bin --slot B --version 1.2.3 \
                   --private ~/keys/ota_release.pem --out gateway_slot_b.ota
```

## Key handling

* **The release private key never enters git.** Generate it outside the
  repository: `make_image.py genkey --private ~/keys/ota_release.pem`.
  The command refuses to overwrite an existing file and sets mode 0600.
* `genkey` also rewrites
  `firmware/gateway/middleware/firmware_store/ota_public_key.c`. That file is
  committed as an **all-zero placeholder**, which is not a point on the curve,
  so verification fails closed until a real key is provisioned.
* Losing the private key means no further OTA update can be accepted by
  already-deployed devices. Back it up.

## Test vectors

`make_image.py vectors` regenerates
`tests/gateway/middleware/firmware_store/vectors/test_vectors.h` and, only if
absent, two throw-away TEST keys beside it. Those keys are committed on purpose:
they sign nothing but the host unit-test images.

Requires the Python `cryptography` package.

## Running an update (MQTT)

`scripts/ota_server.py` is the operator and the file server of a real update: it publishes the
command, answers the device's chunk requests from the signed image of the slot it asked for, and
waits for the terminal report. The wire contract is `docs/lld/application/update-service-lld.md`
§6.

```
scripts/ota_server.py --host <broker> --device-id <24 hex chars> \
    --image-a gateway_slot_a.ota --image-b gateway_slot_b.ota --job 42 \
    --cafile ca.crt --cert operator.crt --key operator.key --expect success
```

`--job` must be unique per update; sending the same job again resumes an interrupted download
(US-D14). `--silent-at`, `--drop-requests` and `--corrupt-body` bend the server for the failure
cases; `--selftest` checks the framing without a broker. For the debug-UART bench harness use
`scripts/ota_uart_server.py` instead.

## Who may publish (REQ-DM-056, US-O5)

MQTT 3.1.1 does not carry the publisher's identity to subscribers, so the device cannot check who
sent a command. Authentication is layered: the TLS session with the genuine broker, **the broker's
authorisation of who may publish to the OTA topics (this section)**, and the signature on the image,
which makes a forged command harmless (it can at worst start a download that ends in `sig_invalid`).

Only the operator identity may publish to `cmd/iotmonitor/+/ota` and `cmd/iotmonitor/+/ota/data`. A
device may read its own command topics and publish only under its own `dt/iotmonitor/<id>/`.

**Mosquitto** (`use_identity_as_username true`, the certificate common name is the user name):

```
user ota-operator
topic write cmd/iotmonitor/+/ota
topic write cmd/iotmonitor/+/ota/data
topic read  dt/iotmonitor/+/ota/request
topic read  dt/iotmonitor/+/ota/result

pattern read  cmd/iotmonitor/%c/#
pattern write dt/iotmonitor/%c/#
```

**AWS IoT Core** (operator policy; the device policy mirrors the `pattern` lines with
`${iot:ClientId}`):

```json
{
  "Version": "2012-10-17",
  "Statement": [
    { "Effect": "Allow", "Action": "iot:Connect",
      "Resource": "arn:aws:iot:<region>:<account>:client/ota-operator" },
    { "Effect": "Allow", "Action": "iot:Publish",
      "Resource": ["arn:aws:iot:<region>:<account>:topic/cmd/iotmonitor/*/ota",
                   "arn:aws:iot:<region>:<account>:topic/cmd/iotmonitor/*/ota/data"] },
    { "Effect": "Allow", "Action": "iot:Subscribe",
      "Resource": ["arn:aws:iot:<region>:<account>:topicfilter/dt/iotmonitor/*/ota/request",
                   "arn:aws:iot:<region>:<account>:topicfilter/dt/iotmonitor/*/ota/result"] },
    { "Effect": "Allow", "Action": "iot:Receive",
      "Resource": ["arn:aws:iot:<region>:<account>:topic/dt/iotmonitor/*/ota/request",
                   "arn:aws:iot:<region>:<account>:topic/dt/iotmonitor/*/ota/result"] }
  ]
}
```

The bring-up broker (`scripts/start-mosquitto-tls.sh`) runs with `allow_anonymous true` and no ACL,
so it does **not** enforce this; TC-SYS-OTA-004 needs the ACL above.
