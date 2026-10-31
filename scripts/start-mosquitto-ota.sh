#!/usr/bin/env bash
# scripts/start-mosquitto-ota.sh - the bring-up Mosquitto broker with the OTA access list.
#
# Same broker certificates as scripts/start-mosquitto-tls.sh (the server certificate must match the
# broker address, see that script), but identity-based authentication and tools/ota/mosquitto/acl.conf:
# only the "ota-operator" certificate (scripts/make-operator-cert.sh) may publish OTA commands.
#
# Usage: scripts/start-mosquitto-ota.sh          foreground; Ctrl-C stops it (container removed)
#        DETACH=1 scripts/start-mosquitto-ota.sh   background; stop with: docker stop ota-mosquitto
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CERTS="$REPO_ROOT/local-test/mosquitto-broker"
CONF="$REPO_ROOT/tools/ota/mosquitto"
NAME="${NAME:-ota-mosquitto}"
PORT="${PORT:-8883}"

for f in ca.crt server.crt server.key; do
    [ -f "$CERTS/$f" ] || { echo "missing $CERTS/$f (scripts/regenerate-bringup-certs.sh)" >&2; exit 1; }
done

mode=(--rm -i)
[ -t 0 ] && mode=(--rm -it)
[ -n "${DETACH:-}" ] && mode=(--rm -d)

echo "Mosquitto with the OTA ACL on port $PORT (container $NAME)"
exec docker run "${mode[@]}" --name "$NAME" -p "$PORT:8883" \
    -v "$CERTS:/mosquitto/certs:ro" \
    -v "$CONF/mosquitto_ota.conf:/mosquitto/config/mosquitto.conf:ro" \
    -v "$CONF/acl.conf:/mosquitto/config/acl.conf:ro" \
    eclipse-mosquitto:2 mosquitto -c /mosquitto/config/mosquitto.conf -v
