#!/usr/bin/env bash
# Linux port of local-test/mosquitto-broker/start-mosquitto-tls.bat: runs
# the bring-up Mosquitto broker (TLS 1.2, mutual auth, port 8883) in the
# eclipse-mosquitto Docker image, so nothing needs installing with sudo.
# Serves local-test/mosquitto-broker/{ca.crt,server.crt,server.key}.
#
# The server cert's SAN is fixed to EXPECTED_IP (baked in by the last
# scripts/regenerate-bringup-certs.sh run). If the broker interface's
# current IPv4 does not match, the board's TLS handshake fails hostname
# verification at TC-HW-MQTT-004 / TC-HW-CP-004 — regenerate first.
#
# Usage: scripts/start-mosquitto-tls.sh [iface]   (default: wlo1)
#   Ctrl-C stops the broker; the container is removed on exit.
set -euo pipefail

EXPECTED_IP=192.168.0.218
IFACE="${1:-wlo1}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BROKER_DIR="$REPO_ROOT/local-test/mosquitto-broker"

CURRENT_IP="$(ip -4 -o addr show "$IFACE" 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)"
echo "Current $IFACE IPv4 address: ${CURRENT_IP:-<none>}"
if [ "$CURRENT_IP" != "$EXPECTED_IP" ]; then
    echo "WARNING: server.crt SAN is $EXPECTED_IP but $IFACE is ${CURRENT_IP:-down}."
    echo "         Run: scripts/regenerate-bringup-certs.sh ${CURRENT_IP:-<ip>}   then rebuild + reflash."
fi

# Container-path twin of mosquitto_tls.conf (which holds Windows paths).
cat > "$BROKER_DIR/mosquitto_tls.docker.conf" <<CONF
listener 8883 0.0.0.0
cafile /mosquitto/certs/ca.crt
certfile /mosquitto/certs/server.crt
keyfile /mosquitto/certs/server.key
require_certificate true
use_identity_as_username false
allow_anonymous true
tls_version tlsv1.2
CONF

echo "Starting Mosquitto on port 8883 (TLS, mutual auth) in Docker..."
exec docker run --rm -it --name bringup-mosquitto -p 8883:8883 \
    -v "$BROKER_DIR:/mosquitto/certs:ro" \
    -v "$BROKER_DIR/mosquitto_tls.docker.conf:/mosquitto/config/mosquitto.conf:ro" \
    eclipse-mosquitto:2 mosquitto -c /mosquitto/config/mosquitto.conf -v
