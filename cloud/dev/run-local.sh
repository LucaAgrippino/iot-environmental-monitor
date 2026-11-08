#!/usr/bin/env bash
# Run the whole cloud side on this machine: a local broker, the backend, and (optionally) a simulated device.
#   cloud/dev/run-local.sh            broker + backend on http://127.0.0.1:8000
#   cloud/dev/run-local.sh --sim      also a simulated device sending a message every few seconds
# The login is off here (the stack listens on localhost only). To try it: IOTCLOUD_AUTH=on cloud/dev/run-local.sh, after
#   python -m iotcloud.admin --db cloud/dev/dev.sqlite3 add-user me --role admin
# Needs docker (for the broker) and the python environment: python3 -m venv ~/.venvs/envmon-cloud &&
#   ~/.venvs/envmon-cloud/bin/pip install -e 'cloud/backend[test]'
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
py="${CLOUD_PY:-$HOME/.venvs/envmon-cloud/bin/python}"
name=iotcloud-dev-mosquitto
docker rm -f "$name" >/dev/null 2>&1 || true
docker run -d --rm --name "$name" -p 127.0.0.1:1883:1883 \
    -v "$root/cloud/dev/mosquitto-dev.conf:/mosquitto/config/mosquitto.conf:ro" eclipse-mosquitto:2 >/dev/null
trap 'kill $(jobs -p) 2>/dev/null; docker rm -f '"$name"' >/dev/null 2>&1' EXIT
sleep 1
if [ "${1:-}" = "--sim" ]; then
    env -u PYTHONPATH "$py" "$root/cloud/sim/device_sim.py" --fast &
fi
cd "$root/cloud/backend"
env -u PYTHONPATH IOTCLOUD_AUTH="${IOTCLOUD_AUTH:-off}" IOTCLOUD_DB="${IOTCLOUD_DB:-$root/cloud/dev/dev.sqlite3}" "$py" -m uvicorn --factory iotcloud.api:app --host 127.0.0.1 --port 8000
