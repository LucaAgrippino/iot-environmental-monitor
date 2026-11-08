#!/usr/bin/env bash
# Try the installer on this machine, in a Debian 13 container (the Pi runs Debian 13 with the same Python), without
# touching the Pi: install from the bundle, create a user, start the service as the iotcloud user, log in over HTTP.
# Not covered: systemd and Caddy (they need a real machine), and the arm64 CPU (the packages are the same, built for arm64).
#   cloud/deploy/test-install.sh            run the check
#   cloud/deploy/test-install.sh --freeze   also write cloud/deploy/requirements.lock from what pip installed
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build-bundle.sh" >/dev/null
freeze=0; [ "${1:-}" = "--freeze" ] && freeze=1
docker run --rm -v "$here/out:/in:ro" -v "$here:/out" -e FREEZE=$freeze debian:trixie bash -euo pipefail -c '
    apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq sudo curl >/dev/null
    cd /tmp && tar xzf /in/iotcloud-bundle.tar.gz && cd iotcloud-bundle
    ./install.sh --no-caddy --no-systemd
    # 1. the admin wrapper creates a user (the password comes from the environment, not from a prompt)
    PW="a long test password" iotcloud-admin add-user tester --role admin --password-env PW
    iotcloud-admin list
    # 2. the service starts as the unprivileged user, with the settings file the installer wrote
    (set -a; . /etc/iotcloud/iotcloud.env; set +a
     IOTCLOUD_MQTT_PORT=1883 IOTCLOUD_MQTT_CA= IOTCLOUD_MQTT_CERT= IOTCLOUD_MQTT_KEY= IOTCLOUD_COOKIE_SECURE=off \
     sudo -u iotcloud --preserve-env env -C /var/lib/iotcloud /opt/iotcloud/venv/bin/python -m uvicorn --factory iotcloud.api:app \
         --host 127.0.0.1 --port 8000 >/tmp/svc.log 2>&1 &)
    for i in $(seq 30); do curl -sf localhost:8000/api/healthz >/dev/null && break; sleep 1; done
    echo "health:        $(curl -s localhost:8000/api/healthz)"
    echo "no login:      $(curl -s -o /dev/null -w %{http_code} localhost:8000/api/devices)"
    echo "web app:       $(curl -s localhost:8000/ | grep -o "<title>[^<]*")"
    curl -s -c /tmp/jar -o /dev/null -w "login:         %{http_code}\n" -X POST localhost:8000/api/auth/login -d "{\"username\":\"tester\",\"password\":\"a long test password\"}"
    echo "with login:    $(curl -s -b /tmp/jar -o /dev/null -w %{http_code} localhost:8000/api/devices)"
    ls -ld /var/lib/iotcloud | cut -c1-40
    [ "$FREEZE" = 1 ] && /opt/iotcloud/venv/bin/pip freeze | grep -vi "^iotcloud" > /out/requirements.lock
    echo OK
'
