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
