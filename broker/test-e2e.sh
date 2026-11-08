#!/usr/bin/env bash
# The whole chain through the secured broker: simulated device (certificate sim-0001) -> Mosquitto with our ACL ->
# backend (certificate iotcloud) -> API, and a command and an OTA back the other way. Needs docker.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$here/.."
py="${CLOUD_PY:-$HOME/.venvs/envmon-cloud/bin/python}"
work="$(mktemp -d)"; name="iotpki-e2e-$$"
cleanup() { kill $(jobs -p) 2>/dev/null || true; docker rm -f "$name" >/dev/null 2>&1 || true; rm -rf "$work"; }
trap cleanup EXIT
export IOTPKI_DIR="$work/pki"
"$py" "$here/iotpki.py" init >/dev/null
"$py" "$here/iotpki.py" server --san DNS:localhost --san IP:127.0.0.1 >/dev/null
"$py" "$here/iotpki.py" service iotcloud >/dev/null
"$py" "$here/iotpki.py" device sim-0001 --allow-sim >/dev/null
"$py" "$here/iotpki.py" export-broker --out "$work/iot" >/dev/null
sed 's|/etc/mosquitto/iot|/mosquitto/iot|; s|/var/lib/mosquitto/|/mosquitto/data/|' "$here/mosquitto/mosquitto.conf" > "$work/iot/mosquitto.conf"
cp "$here/mosquitto/acl.conf" "$work/iot/"; mkdir "$work/data"
docker run -d --rm --name "$name" -u "$(id -u):$(id -g)" -p 127.0.0.1:18884:8883 -v "$work/iot:/mosquitto/iot:ro" \
    -v "$work/data:/mosquitto/data" eclipse-mosquitto:2 mosquitto -c /mosquitto/iot/mosquitto.conf >/dev/null
sleep 2
c="$work/pki/issued"
(cd "$repo/cloud/backend" && env -u PYTHONPATH IOTCLOUD_AUTH=off IOTCLOUD_DB="$work/db.sqlite3" IOTCLOUD_IMAGE_DIR="$work/img" \
    IOTCLOUD_MQTT_HOST=localhost IOTCLOUD_MQTT_PORT=18884 IOTCLOUD_MQTT_CA="$c/iotcloud/ca.crt" \
    IOTCLOUD_MQTT_CERT="$c/iotcloud/iotcloud.crt" IOTCLOUD_MQTT_KEY="$c/iotcloud/iotcloud.key" \
    "$py" -m uvicorn --factory iotcloud.api:app --port 18000 >"$work/api.log" 2>&1) &
env -u PYTHONPATH "$py" "$repo/cloud/sim/device_sim.py" --fast --host localhost --port 18884 --cafile "$c/sim-0001/ca.crt" \
    --cert "$c/sim-0001/sim-0001.crt" --key "$c/sim-0001/sim-0001.key" >"$work/sim.log" 2>&1 &
A=http://127.0.0.1:18000
for i in $(seq 40); do curl -sf $A/api/devices | grep -q sim-0001 && break; sleep 1; done
echo "device seen by the cloud: $(curl -s $A/api/devices | grep -o '"id":"sim-0001"')"
curl -s -X POST $A/api/devices/sim-0001/control -d '{"command":"read"}' >/dev/null
head -c 20000 /dev/urandom > "$work/a.ota"; head -c 15000 /dev/urandom > "$work/b.ota"
curl -s -X PUT --data-binary @"$work/a.ota" $A/api/images/a.ota >/dev/null; curl -s -X PUT --data-binary @"$work/b.ota" $A/api/images/b.ota >/dev/null
curl -s -X POST $A/api/devices/sim-0001/ota -d '{"image_a":"a.ota","image_b":"b.ota"}' >/dev/null
sleep 6
cmd=$(curl -s $A/api/devices/sim-0001/commands | grep -o '"status":"[a-z]*"' | head -1)
ota=$(curl -s $A/api/devices/sim-0001/ota | grep -o '"state":"[a-z_]*"' | head -1)
echo "command: $cmd   ota: $ota"
[ "$cmd" = '"status":"accepted"' ] && [ "$ota" = '"state":"success"' ] && echo OK || { echo FAIL; tail -5 "$work/api.log" "$work/sim.log"; exit 1; }
