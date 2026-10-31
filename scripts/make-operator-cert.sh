#!/usr/bin/env bash
# scripts/make-operator-cert.sh - create the OTA operator client certificate for the bench broker.
#
# The operator identity is the certificate common name "ota-operator", which tools/ota/mosquitto/acl.conf
# allows to publish to the OTA command topics. It is signed by the bring-up CA in
# local-test/mosquitto-broker (the same CA as the device and server certificates, so the existing
# broker trusts it). Nothing here is committed: local-test/ is git-ignored.
#
# Usage: scripts/make-operator-cert.sh        -> local-test/mosquitto-broker/operator.{crt,key}
set -euo pipefail

BROKER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/local-test/mosquitto-broker"
cd "$BROKER_DIR"
[ -f ca.crt ] && [ -f ca.key ] || { echo "no bring-up CA in $BROKER_DIR (see scripts/regenerate-bringup-certs.sh)" >&2; exit 1; }
if [ -f operator.key ]; then echo "operator.key exists, keeping it"; exit 0; fi

openssl req -new -newkey rsa:2048 -nodes -keyout operator.key -out operator.csr -subj "/CN=ota-operator" 2>/dev/null
openssl x509 -req -in operator.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out operator.crt -days 3650 2>/dev/null
rm -f operator.csr
chmod 600 operator.key
echo "operator.crt and operator.key written to $BROKER_DIR"
openssl x509 -in operator.crt -noout -subject
