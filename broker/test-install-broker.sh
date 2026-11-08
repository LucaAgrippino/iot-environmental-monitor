#!/usr/bin/env bash
# Try the broker installer in a Debian 13 container (the Pi's distribution and Mosquitto version): a fake bench
# configuration is in place, the installer replaces it, Mosquitto starts with the real Debian files, a device and the
# cloud talk through it, and --rollback brings the bench configuration back. Nothing touches the Pi.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
export IOTPKI_DIR="$work/pki"
py="${CLOUD_PY:-$HOME/.venvs/envmon-cloud/bin/python}"
SERIAL=002d004f3442501020323848
"$py" "$here/iotpki.py" init >/dev/null
"$py" "$here/iotpki.py" server --san DNS:localhost --san IP:127.0.0.1 >/dev/null
"$py" "$here/iotpki.py" service iotcloud >/dev/null
"$py" "$here/iotpki.py" device $SERIAL >/dev/null
PATH="$(dirname "$(command -v "$py")"):$PATH" "$here/build-bundle.sh" >/dev/null
cp -r "$IOTPKI_DIR/issued" "$work/issued"
docker run --rm -v "$here/out:/in:ro" -v "$work/issued:/issued:ro" -e S=$SERIAL debian:trixie bash -euo pipefail -c '
    apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq mosquitto mosquitto-clients procps >/dev/null
    mosquitto -h | head -1 || true
    # stand-in for the bench configuration of the Pi
    printf "listener 8883 0.0.0.0\nallow_anonymous true\n" > /etc/mosquitto/conf.d/bench.conf
    cd /tmp && tar xzf /in/broker-bundle.tar.gz && cd broker-bundle
    ./install-broker.sh --replace-bench --no-systemd
    ls /etc/mosquitto/conf.d/
    mosquitto -c /etc/mosquitto/mosquitto.conf -d; sleep 2
    ssl() { echo --cafile /issued/$1/ca.crt --cert /issued/$1/$1.crt --key /issued/$1/$1.key -h localhost -p 8883 --tls-version tlsv1.2; }
    mosquitto_sub $(ssl iotcloud) -i iotcloud -t "dt/iotmonitor/+/#" -C 1 -W 6 > /tmp/got &
    sleep 1
    mosquitto_pub $(ssl $S) -i $S -t dt/iotmonitor/$S/telemetry -m hello -q 1
    wait
    echo "cloud received: $(cat /tmp/got)"
    [ "$(cat /tmp/got)" = hello ]
    if mosquitto_pub -h localhost -p 8883 --cafile /issued/iotcloud/ca.crt -t x -m y 2>/dev/null; then echo "FAIL: anonymous accepted"; exit 1; else echo "anonymous refused"; fi
    pkill mosquitto; sleep 1
    ./install-broker.sh --rollback --no-systemd
    ls /etc/mosquitto/conf.d/ && echo OK
'
