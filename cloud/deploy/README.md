# Deploying the cloud on the Raspberry Pi

What runs on the Pi, and how it maps to the AWS services:

```text
browser ──https──> Caddy :443 ──http──> iotcloud :8000 (127.0.0.1) ──mqtt/tls──> Mosquitto :8883 <── the Gateway
 [CloudFront/ALB]  [TLS, certificate]    [API Gateway + Lambda + DB]               [IoT Core]
```

| File | What it is |
|---|---|
| `build-bundle.sh` | on the PC: packs backend, built web app, service files and installer into `out/iotcloud-bundle.tar.gz` |
| `install.sh` | on the Pi: installs or upgrades everything, keeps data and settings |
| `iotcloud.service` | the systemd unit (unprivileged user, read-only system, writes only `/var/lib/iotcloud`; 300 MB memory cap, see below) |
| `iotcloud.env.example` | the settings file, copied to `/etc/iotcloud/iotcloud.env` on first install |
| `Caddyfile` | HTTPS in front of the service, with Caddy's own certificate authority |
| `requirements.lock` | exact Python package versions (made by `test-install.sh --freeze`) |
| `test-install.sh` | tries the installer in a Debian 13 container on the PC |

## Before you touch the Pi

- **Do not install while a verification run is using the Pi.** The Pi has 1 GB of RAM, and `install.sh` runs `apt`
  and `pip`, which compete with it. Wait for the endurance run to finish.
- **The broker is a separate step.** The Mosquitto on the Pi is the bench broker: one listener on 8883 with mutual
  TLS, certificates from the bench CA, and `allow_anonymous true`. The cloud connects to it as a client, so it needs a
  client certificate. `broker/` has the system's own CA, an access list and an installer that replaces the bench
  configuration (see `broker/README.md`, including what stops working when you do). Until then the cloud can use a
  certificate from the bench CA.

## Install

On the PC:

```sh
cloud/web/build.sh                  # once, and after a change in cloud/web/src
cloud/deploy/test-install.sh        # optional: runs the installer in a Debian 13 container
cloud/deploy/build-bundle.sh
scp cloud/deploy/out/iotcloud-bundle.tar.gz luca@192.168.0.209:
```

On the Pi:

```sh
tar xzf iotcloud-bundle.tar.gz && cd iotcloud-bundle && sudo ./install.sh
sudo iotcloud-admin add-user luca --role admin        # asks for a password (at least 10 characters)
sudo iotcloud-admin add-user guest --role viewer      # optional: can read, cannot send anything
sudo nano /etc/iotcloud/iotcloud.env                  # broker address and certificate paths
sudo install -o root -g iotcloud -m 0640 ca.crt cloud.crt cloud.key /etc/iotcloud/certs/
sudo systemctl restart iotcloud
```

Check: `systemctl status iotcloud`, `journalctl -u iotcloud -f`, and `curl -s localhost:8000/api/healthz`.

## First install, before the broker step

The service needs a client certificate for the broker, and `iotcloud.env.example` points at
`/etc/iotcloud/certs/{ca.crt,iotcloud.crt,iotcloud.key}`. Until the system's own CA is in use (`broker/`), the Pi's bench
broker accepts the bench certificates it already has. Copy them, on the Pi (no key leaves it):

```sh
b=/etc/mosquitto/bench-certs
sudo install -m 0640 -o root -g iotcloud $b/ca.crt       /etc/iotcloud/certs/ca.crt
sudo install -m 0640 -o root -g iotcloud $b/operator.crt /etc/iotcloud/certs/iotcloud.crt
sudo install -m 0640 -o root -g iotcloud $b/operator.key /etc/iotcloud/certs/iotcloud.key
sudo systemctl restart iotcloud
```

This is a stand-in: the cloud then connects as the bench's `ota-operator` identity, with no access list in force. If the
files are missing the service still starts (the web app and the stored data work, commands answer 503) and the journal
says which file is missing: `journalctl -u iotcloud -n 20`.

## HTTPS and the browser warning

Caddy issues a certificate for `envmon.lan` from its own authority, so each browser must trust that authority once:

```sh
ssh luca@192.168.0.209 'sudo cat /var/lib/caddy/.local/share/caddy/pki/authorities/local/root.crt' > envmon-root.crt
```

Import `envmon-root.crt` as a trusted authority in the browser (or the operating system). The name `envmon.lan` must
resolve to the Pi: add `192.168.0.209 envmon.lan` to the PC's `/etc/hosts`, or set `IOTCLOUD_HOST` in Caddy's
environment to a name your router knows. Reaching it from outside the house is a separate decision (a VPN such as
WireGuard is the safe way); nothing here opens a port to the internet.

## The memory cap is not enforced on Raspberry Pi OS

`MemoryMax=300M` in the unit needs the kernel's memory controller, and the Pi's kernel boots with
`cgroup_disable=memory`. Check: `cat /sys/fs/cgroup/cgroup.controllers` lists no `memory`. The service uses about
60 MB, so nothing is wrong today; the cap only matters as a guard. To make it real, add `cgroup_enable=memory
cgroup_memory=1` to the single line of `/boot/firmware/cmdline.txt` and reboot (not done here: it reboots the Pi).

## Upgrade, back up, remove

- Upgrade: build a new bundle, copy it, run `sudo ./install.sh` again. The database, images, users and settings stay.
- Back up: `/var/lib/iotcloud` (the SQLite database and the images) and `/etc/iotcloud`. The database is a single
  file in WAL mode: `sqlite3 /var/lib/iotcloud/iotcloud.sqlite3 ".backup /tmp/iotcloud.bak"` copies it while running.
- Remove: `sudo systemctl disable --now iotcloud`, then delete `/opt/iotcloud`, `/etc/iotcloud`, the unit file and
  (if you want the data gone) `/var/lib/iotcloud`.

## What was tested without the Pi

`test-install.sh` installs from the bundle in a Debian 13 container (the same Python 3.13 as the Pi), creates a user,
starts the service as the unprivileged user and checks: health, 401 without a login, the web app, login, and the API
with the session. The unit file passes `systemd-analyze verify` and the Caddyfile passes `caddy validate`.
**Not tested:** systemd and Caddy running for real, and the arm64 CPU. The first install on the Pi is the first real
run of those; expect to read the journal once.
