#!/usr/bin/env bash
# Put the system's broker configuration on the Raspberry Pi: our CA, the broker certificate, the access list and the
# revocation list, in place of the bench broker's settings. Run as root from the unpacked bundle (build-bundle.sh).
#
#   sudo ./install-broker.sh --replace-bench     install (the bench configuration is kept as bench.conf.disabled)
#   sudo ./install-broker.sh --update-crl        after a revocation: install the new list and reload (no restart)
#   sudo ./install-broker.sh --rollback          back to the bench configuration
#
# WARNING: after --replace-bench the bench certificates stop working. The Gateway, the Field Device harness and any
# script that connects to this broker need certificates from the new CA (iotpki.py device ...) first.
# Flag --no-systemd: do not start or stop anything (for the container test).
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mode=""; systemd=1
for a in "$@"; do
    case "$a" in
        --replace-bench|--update-crl|--rollback) mode="$a" ;;
        --no-systemd) systemd=0 ;;
        *) echo "unknown option $a"; exit 1 ;;
    esac
done
[ -n "$mode" ] || { sed -n 2,11p "$0"; exit 1; }
conf=/etc/mosquitto/conf.d; iot=/etc/mosquitto/iot
restart() {
    [ "$systemd" = 1 ] || return 0
    systemctl restart mosquitto && sleep 1 && systemctl is-active --quiet mosquitto
}

case "$mode" in
--rollback)
    [ -f "$conf/bench.conf.disabled" ] || { echo "nothing to roll back to"; exit 1; }
    rm -f "$conf/iot.conf"
    mv "$conf/bench.conf.disabled" "$conf/bench.conf"
    restart && echo "bench configuration restored"
    ;;
--update-crl)
    install -m 0644 -o root -g mosquitto "$here/certs/crl.pem" "$iot/crl.pem"
    [ "$systemd" = 1 ] && systemctl reload mosquitto
    echo "revocation list installed; connected clients keep their sessions, new handshakes see the list"
    ;;
--replace-bench)
    command -v mosquitto >/dev/null || { echo "mosquitto is not installed (apt install mosquitto)"; exit 1; }
    for f in ca.crt server.crt server.key crl.pem; do [ -f "$here/certs/$f" ] || { echo "missing certs/$f"; exit 1; }; done
    install -d -m 0750 -o root -g mosquitto "$iot"
    install -m 0644 -o root -g mosquitto "$here/certs/ca.crt" "$here/certs/server.crt" "$here/certs/crl.pem" "$iot/"
    install -m 0640 -o root -g mosquitto "$here/certs/server.key" "$iot/server.key"
    install -m 0640 -o root -g mosquitto "$here/acl.conf" "$iot/acl.conf"
    # The distribution's main file already sets persistence and logging: keep only our listener and security lines
    grep -v -E '^(persistence|log_type)' "$here/mosquitto.conf" > "$conf/iot.conf"
    chmod 0644 "$conf/iot.conf"
    if [ -f "$conf/bench.conf" ]; then mv "$conf/bench.conf" "$conf/bench.conf.disabled"; fi
    if ! restart; then
        echo "Mosquitto did not start with the new configuration: rolling back"
        rm -f "$conf/iot.conf"; [ -f "$conf/bench.conf.disabled" ] && mv "$conf/bench.conf.disabled" "$conf/bench.conf"
        [ "$systemd" = 1 ] && systemctl restart mosquitto
        exit 1
    fi
    echo "broker installed: mutual TLS on 8883, certificates from our CA, access list active"
    ;;
esac
