#!/usr/bin/env bash
# Alerts, for real: a simulated device raises an alarm and then goes silent; a real ntfy server (Docker) receives the
# push messages the backend sends. Needs docker. Takes about a minute.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
py="${CLOUD_PY:-$HOME/.venvs/envmon-cloud/bin/python}"
work="$(mktemp -d)"; b="alerts-e2e-broker-$$"; n="alerts-e2e-ntfy-$$"
cleanup() { kill $(jobs -p) 2>/dev/null || true; docker rm -f "$b" "$n" >/dev/null 2>&1 || true; rm -rf "$work"; }
trap cleanup EXIT
docker run -d --rm --name "$b" -p 127.0.0.1:18885:1883 -v "$root/cloud/dev/mosquitto-dev.conf:/mosquitto/config/mosquitto.conf:ro" eclipse-mosquitto:2 >/dev/null
docker run -d --rm --name "$n" -p 127.0.0.1:18886:80 binwiederhier/ntfy serve >/dev/null
sleep 3
# a short online window so that "silent" happens within the test
(cd "$root/cloud/backend" && exec env -u PYTHONPATH IOTCLOUD_AUTH=off IOTCLOUD_DB="$work/db" IOTCLOUD_MQTT_PORT=18885 \
    IOTCLOUD_ONLINE_WINDOW_S=20 "$py" -m uvicorn --factory iotcloud.api:app --port 18001 >"$work/api.log" 2>&1) &
env -u PYTHONPATH "$py" "$root/cloud/sim/device_sim.py" --fast --drift 1.5 --port 18885 >"$work/sim.log" 2>&1 &
sim=$!
A=http://127.0.0.1:18001
for i in $(seq 30); do curl -sf $A/api/healthz >/dev/null && break; sleep 1; done
topic="envmon-$(head -c 6 /dev/urandom | od -An -tx1 | tr -d ' \n')"
curl -s -X POST $A/api/notify/channels -d "{\"name\":\"phone\",\"kind\":\"ntfy\",\"config\":{\"url\":\"http://127.0.0.1:18886\",\"topic\":\"$topic\"},\"events\":[\"alarm\",\"alarm_clear\",\"device_offline\",\"device_online\"]}" >/dev/null
echo "test button: $(curl -s -X POST $A/api/notify/channels/1/test)"
for i in $(seq 40); do curl -s "http://127.0.0.1:18886/$topic/json?poll=1" | grep -q "HIGH" && break; sleep 2; done
kill $sim; wait $sim 2>/dev/null || true       # the device goes silent
for i in $(seq 40); do curl -s "http://127.0.0.1:18886/$topic/json?poll=1" | grep -q "stopped sending" && break; sleep 2; done
echo "--- what the phone received:"
curl -s "http://127.0.0.1:18886/$topic/json?poll=1" | "$py" -c "
import sys, json
for l in sys.stdin:
    m = json.loads(l)
    if m.get('event') == 'message': print(f\"[p{m.get('priority')}] {m.get('title')}: {m.get('message')}\")"
got=$(curl -s "http://127.0.0.1:18886/$topic/json?poll=1")
echo "$got" | grep -q "Test from" && echo "$got" | grep -q "HIGH" && echo "$got" | grep -q "stopped sending" && echo OK || { echo FAIL; tail -n 8 "$work/api.log"; exit 1; }
