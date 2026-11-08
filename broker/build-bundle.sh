#!/usr/bin/env bash
# Pack the broker files for the Pi: configuration, access list, installer, and the broker's certificates from the CA
# (never the CA key). Run on the PC, after `iotpki.py server --san ...`.
#   broker/build-bundle.sh   -> broker/out/broker-bundle.tar.gz
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
stage="$(mktemp -d)"; trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/broker-bundle"
python3 "$here/iotpki.py" export-broker --out "$stage/broker-bundle/certs"
cp "$here/mosquitto/mosquitto.conf" "$here/mosquitto/acl.conf" "$here/install-broker.sh" "$stage/broker-bundle/"
mkdir -p "$here/out"
tar -C "$stage" -czf "$here/out/broker-bundle.tar.gz" broker-bundle
ls -l "$here/out/broker-bundle.tar.gz"
