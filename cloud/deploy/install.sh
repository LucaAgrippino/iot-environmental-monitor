#!/usr/bin/env bash
# Install or upgrade the cloud service on the Raspberry Pi (Debian 13). Run as root from the unpacked bundle:
#   tar xzf iotcloud-bundle.tar.gz && cd iotcloud-bundle && sudo ./install.sh
# Safe to run again: it upgrades the code and keeps the database, the images, the settings and the users.
# Flags: --no-caddy (no HTTPS front end), --no-systemd (containers and tests: do not start anything)
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run as root (sudo ./install.sh)"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
caddy=1; systemd=1
for a in "$@"; do
    case "$a" in --no-caddy) caddy=0 ;; --no-systemd) systemd=0 ;; *) echo "unknown option $a"; exit 1 ;; esac
done

pkgs="python3-venv"; [ "$caddy" = 1 ] && pkgs="$pkgs caddy"
echo "== packages: $pkgs"
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq $pkgs >/dev/null

echo "== user and folders"
id iotcloud >/dev/null 2>&1 || useradd --system --home-dir /var/lib/iotcloud --shell /usr/sbin/nologin iotcloud
install -d -o iotcloud -g iotcloud -m 0750 /var/lib/iotcloud /var/lib/iotcloud/images
install -d -m 0755 /opt/iotcloud
install -d -o root -g iotcloud -m 0750 /etc/iotcloud /etc/iotcloud/certs

echo "== code"
rm -rf /opt/iotcloud/app /opt/iotcloud/web
cp -r "$here/backend" /opt/iotcloud/app
cp -r "$here/web" /opt/iotcloud/web
[ -d /opt/iotcloud/venv ] || python3 -m venv /opt/iotcloud/venv
constraint=(); [ -f "$here/requirements.lock" ] && constraint=(-c "$here/requirements.lock")
/opt/iotcloud/venv/bin/pip install -q --upgrade "${constraint[@]}" /opt/iotcloud/app

echo "== settings"
if [ ! -f /etc/iotcloud/iotcloud.env ]; then
    install -m 0640 -o root -g iotcloud "$here/iotcloud.env.example" /etc/iotcloud/iotcloud.env
    echo "   created /etc/iotcloud/iotcloud.env: check the broker lines"
fi
cat > /usr/local/bin/iotcloud-admin <<'WRAP'
#!/usr/bin/env bash
# Manage cloud users: iotcloud-admin add-user luca --role admin | list | set-password NAME | delete-user NAME
set -a; . /etc/iotcloud/iotcloud.env; set +a
exec sudo -u iotcloud --preserve-env /opt/iotcloud/venv/bin/python -m iotcloud.admin "$@"
WRAP
chmod 0755 /usr/local/bin/iotcloud-admin

if [ "$systemd" = 1 ]; then
    echo "== services"
    install -m 0644 "$here/iotcloud.service" /etc/systemd/system/iotcloud.service
    if [ "$caddy" = 1 ]; then
        [ -f /etc/caddy/Caddyfile ] && [ ! -f /etc/caddy/Caddyfile.orig ] && cp /etc/caddy/Caddyfile /etc/caddy/Caddyfile.orig
        install -m 0644 "$here/Caddyfile" /etc/caddy/Caddyfile
    fi
    systemctl daemon-reload
    systemctl enable iotcloud >/dev/null
    systemctl restart iotcloud
    [ "$caddy" = 1 ] && systemctl enable caddy >/dev/null && systemctl restart caddy
    sleep 2
    systemctl is-active iotcloud
fi

cat <<MSG

Installed. Next:
  sudo iotcloud-admin add-user <name> --role admin     create your login
  sudo nano /etc/iotcloud/iotcloud.env                 broker address and certificates
  https://envmon.lan/                                  (or the address in IOTCLOUD_HOST); see cloud/deploy/README.md
MSG
