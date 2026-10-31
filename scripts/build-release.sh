#!/usr/bin/env bash
# scripts/build-release.sh - build, link and sign the production Gateway image for both OTA slots.
#
# One release is linked twice, once for Slot A (0x0800_6000) and once for Slot B (0x0808_0000), and
# each body is signed separately (tools/ota/README.md). This is the release step FS-O9 / BL-O7 found
# missing. It uses scripts/build-gateway-app.sh (SLOT=A|B), so it needs the same git-ignored
# firmware/gateway/certs headers and the ARM toolchain, and it checks that the public key compiled
# into the image (firmware_store/ota_public_key.c) belongs to the private key that signs it: an image
# signed with another key is rejected by the device (sig_invalid, TC-SYS-OTA-005).
#
# usage: scripts/build-release.sh --version X.Y.Z --private KEY.pem --out DIR
#                                 [--fail-selfcheck] [--flash] [--bootloader BOOTLOADER.elf]
#   --version X.Y.Z      release version, written into both signed images and the Slot-A descriptor
#   --private KEY.pem    release private key, kept outside the repository (make_image.py genkey)
#   --out DIR            writes gateway_slot_{a,b}.{bin,elf,map,ota} and desc_a.bin there
#   --fail-selfcheck     build with -DGATEWAY_FORCE_SELF_CHECK_FAIL=1: the modbus probe is false, so the
#                        trial self-check fails and the bootloader must roll back (TC-SYS-OTA-002)
#   --flash              after the build, provision the board: erase the boot metadata and the slot
#                        descriptors, program Slot A and its descriptor, reset
#   --flash-only         provision from an existing --out directory without building (no --version or
#                        --private needed)
#   --bootloader FILE    with --flash / --flash-only: also program this bootloader.elf
#                        (scripts/build-bootloader.sh); otherwise it must already be in flash
# Needs the Python package "cryptography".
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
gw="$root/firmware/gateway"
version="" key="" out="" fail_sc=0 flash=0 flash_only=0 bootloader=""
while (( $# )); do
    case "$1" in
        --version) version="$2"; shift 2 ;;
        --private) key="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --fail-selfcheck) fail_sc=1; shift ;;
        --flash) flash=1; shift ;;
        --flash-only) flash=1; flash_only=1; shift ;;
        --bootloader) bootloader="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ -n "$out" && ( $flash_only == 1 || ( -n "$version" && -n "$key" ) ) ]] || { sed -n '/^# usage/,/^# Needs/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
mkdir -p "$out"
out="$(cd "$out" && pwd)"
if (( ! flash_only )); then  # the build half (not indented: it contains a literal heredoc)
[[ -f "$key" ]] || { echo "no such key: $key" >&2; exit 1; }
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "--version must be major.minor.patch" >&2; exit 1; }

# The key compiled into the image must be the public half of the signing key.
python3 - "$key" "$gw/middleware/firmware_store/ota_public_key.c" "$root" <<'PY'
import re, sys
sys.path.insert(0, sys.argv[3] + "/tools/ota")
from cryptography.hazmat.primitives import serialization
import make_image
key = serialization.load_pem_private_key(open(sys.argv[1], "rb").read(), password=None)
want = make_image.raw_public_key(key)
text = open(sys.argv[2]).read()
have = bytes(int(h, 16) for h in re.findall(r"0x([0-9A-Fa-f]{2})", text.split("{", 1)[1]))
if have != want:
    sys.exit("ota_public_key.c does not hold the public key of %s%s" % (
        sys.argv[1], " (it is still the all-zero placeholder)" if not any(have) else ""))
print("public key in the image matches the signing key")
PY

extra="${EXTRA_CFLAGS:-}"
(( fail_sc )) && extra="$extra -DGATEWAY_FORCE_SELF_CHECK_FAIL=1"
for slot in A B; do
    lower="${slot,,}"
    echo "== Slot $slot"
    SLOT="$slot" EXTRA_CFLAGS="$extra" bash "$root/scripts/build-gateway-app.sh"
    build="$gw/Debug/app_slot_${lower}"
    cp "$build/gateway_app.bin" "$out/gateway_slot_${lower}.bin"
    cp "$build/gateway_app.elf" "$out/gateway_slot_${lower}.elf"
    cp "$build/gateway_app.map" "$out/gateway_slot_${lower}.map"
    python3 "$root/tools/ota/make_image.py" sign --body "$out/gateway_slot_${lower}.bin" --slot "$slot" \
        --version "$version" --private "$key" --out "$out/gateway_slot_${lower}.ota"
done

IFS=. read -r major minor patch <<< "$version"
desc_version=$(printf '0x%08x' $(( (major << 24) | (minor << 16) | patch )))
python3 "$root/scripts/make_slot_descriptor.py" "$out/gateway_slot_a.bin" --slot A \
    --version "$desc_version" -o "$out/desc_a.bin"
echo "release $version$( (( fail_sc )) && echo ' (self-check forced to fail)') written to $out"
fi

if (( flash )); then
    p=/opt/st/stm32cubeide_2.2.0/plugins
    cmds=(-c "init" -c "reset halt")
    [[ -z "$bootloader" ]] || cmds+=(-c "program $bootloader verify")
    # 0x0800_4000..0x0800_5FFF: boot metadata, Slot-A and Slot-B descriptors (flash-partition-layout.md 7.1)
    cmds+=(-c "flash erase_address 0x08004000 0x2000"
           -c "program $out/gateway_slot_a.bin 0x08006000 verify"
           -c "program $out/desc_a.bin 0x08005000 verify"
           -c "reset run" -c "exit")
    "$p"/com.st.stm32cube.ide.mcu.externaltools.openocd.linux64_*/tools/bin/openocd \
        -s "$(echo "$p"/com.st.stm32cube.ide.mcu.debug.openocd_*/resources/openocd/st_scripts)" \
        -f "$gw/gateway.cfg" "${cmds[@]}"
fi
