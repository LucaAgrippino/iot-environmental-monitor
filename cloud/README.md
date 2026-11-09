# cloud: the remote side of the IoT Environmental Monitor

The firmware talks to "the cloud" over MQTT with mutual TLS, the way it would talk to AWS IoT Core. This folder is
that cloud, small enough to run on one machine (a PC while developing, the Raspberry Pi 3B in the end), so every
step a real IoT system needs can be seen, run and broken on purpose. AWS is the reference: each piece below names
the AWS service it replaces. An optional last step can bridge the same messages to a real AWS account.

## What exists today

| Piece | State |
|---|---|
| `backend/` ingest of every device message into SQLite | done |
| `backend/` HTTP API: devices, history, alarms, events, **downloads (CSV, JSON)**, live stream | done |
| `sim/device_sim.py`: a device that speaks the firmware's schema 1.1 and answers `/config`, `/control` and an OTA (`--ota-fail` ends it in a failure) | done |
| `dev/run-local.sh`: broker, backend and simulator on this machine | done |
| Commands from the API (config, control): request id, result tracking, timeout | done |
| OTA service: firmware store, jobs, chunk server, progress and result | done |
| `web/` web app (Svelte): devices, live values, charts, alarms, commands, firmware update with progress, downloads | done |
| Login: users, scrypt passwords, sessions, admin and viewer roles, lockout, cross-site check | done, 31 tests in all |
| `deploy/`: bundle, installer, systemd unit, Caddy HTTPS; installer tested in a Debian 13 container | done, not yet run on the Pi |
| `broker/`: own CA, access list by certificate, revocation, installer for the Pi's broker | done, see `broker/README.md` |
| Alerts: ntfy (phone push), webhook, e-mail; per-channel events, retries, rate limit, offline watcher, log | done |

## Layout

```text
cloud/
  backend/    Python package `iotcloud`: ingest, database, API (FastAPI). Tests in backend/tests.
  sim/        the simulated device
  dev/        run the whole thing on this machine
  web/        the web app (Svelte)
  deploy/     installer, systemd unit and Caddy configuration for the Raspberry Pi
  doc/        Cloud-Guide.pdf: what everything here is and how the pieces connect, for a reader new to cloud systems
```

The broker side (certificate authority, Mosquitto configuration and access list) is the top-level `broker/` folder.

It lives in this repository because the messages are a contract with the firmware
(`firmware/gateway/application/cloud_publisher/`): a change on one side must be seen on the other. It can move to
its own repository later without changes, since nothing here imports firmware code.

## AWS reference and what replaces it

| AWS | Here |
|---|---|
| IoT Core broker, X.509 certificates, policies | Mosquitto with mutual TLS and an ACL file (the broker already runs on the Pi) |
| Registry of things | `devices` table, filled when a device is first heard |
| Rules engine | `iotcloud/ingest.py`: validates and stores each message |
| Timestream, DynamoDB | SQLite (PostgreSQL later, to compare) |
| Device Shadow (desired and reported state) | the `config` events and the latest health report, with request ids |
| IoT Jobs (OTA) | the OTA service: job records, chunk server, result tracking |
| CloudWatch Logs | the `events` table and the downloads; device logs need firmware support (see below) |
| SNS | alerts: `backend/iotcloud/notify.py` (ntfy, webhook, e-mail; channels with an event filter) |
| API Gateway, Lambda | the FastAPI app |
| Cognito | login on the web app, users and roles in SQLite (`backend/iotcloud/auth.py`) |
| Amplify, CloudFront | Caddy serving the web app over HTTPS (later) |

## Topics (from the firmware)

```text
device -> cloud   dt/iotmonitor/<device>/telemetry   every 60 s     QoS 0
                  dt/iotmonitor/<device>/health      every 5 min     QoS 0
                  dt/iotmonitor/<device>/alarms      on change       QoS 1
                  dt/iotmonitor/<device>/config/result, control/result, ota/result     QoS 1
                  dt/iotmonitor/<device>/ota/request                  one per image chunk
cloud -> device   cmd/iotmonitor/<device>/config     {"temp_alarm_high":3000,"req":5}
                  cmd/iotmonitor/<device>/control    {"command":"read|restart|restart_confirm|reset_metrics","req":7}
                  cmd/iotmonitor/<device>/ota        {"job":42,"size_a":N,"size_b":N}
                  cmd/iotmonitor/<device>/ota/data   binary chunk (job, offset, bytes)
```

Every message carries `schema_version` "1.1", `device_id`, `timestamp` and `sync_state`. A device whose clock is not
synchronised has its time replaced by the time of reception.

## The web app

`web/` is a Svelte app, built into plain files that the backend serves at `/`. Node is not needed on this machine:
`cloud/web/build.sh` runs the build in a `node:20-alpine` container and writes `cloud/web/dist`. Run it once, then
`cloud/dev/run-local.sh --sim` and open <http://127.0.0.1:8000>. After a change to `web/src`, run `build.sh` again.
A link such as `http://127.0.0.1:8000/#sim-0001/Firmware` opens a device on a given tab.

## Run it on this machine

```sh
python3 -m venv ~/.venvs/envmon-cloud
~/.venvs/envmon-cloud/bin/pip install -e 'cloud/backend[test]'
cloud/dev/run-local.sh --sim          # http://127.0.0.1:8000/docs shows every endpoint
```

Tests: `cd cloud/backend && env -u PYTHONPATH ~/.venvs/envmon-cloud/bin/python -m pytest`
(the `env -u PYTHONPATH` keeps a ROS installation, if any, out of pytest).

Examples:

```sh
curl http://127.0.0.1:8000/api/devices
curl 'http://127.0.0.1:8000/api/devices/sim-0001/telemetry?limit=10'
curl -OJ 'http://127.0.0.1:8000/api/devices/sim-0001/export?kind=alarms&format=csv'
curl -N http://127.0.0.1:8000/api/stream          # live events

# commands: the answer is a row you can poll, and an event on the stream
curl -X POST localhost:8000/api/devices/sim-0001/config  -d '{"telemetry_interval_ms":10000}'
curl -X POST localhost:8000/api/devices/sim-0001/control -d '{"command":"read"}'
curl localhost:8000/api/devices/sim-0001/commands      # status: pending, applied/accepted, rejected (+ reason), timeout

# OTA: upload one image per flash slot (tools/ota/make_image.py), start the job, follow it
curl -X PUT --data-binary @a.ota localhost:8000/api/images/a.ota
curl -X PUT --data-binary @b.ota localhost:8000/api/images/b.ota
curl -X POST localhost:8000/api/devices/sim-0001/ota -d '{"image_a":"a.ota","image_b":"b.ota"}'
curl localhost:8000/api/devices/sim-0001/ota           # state: sent, running, success/failed/rolled_back/rejected
curl -X POST localhost:8000/api/ota/1/cancel           # stop serving chunks
```

## Alerts to the phone

Open **Alerts** in the web app, add a channel, press **Test**. For a phone: install the ntfy app (Android or iOS),
subscribe to the same server and topic. Which server:

- **Your own ntfy** (a small Go program; a container on the PC, or the Pi): messages stay in the house, and they
  reach the phone only while it is on the home network or a VPN. Run one to try it:
  `docker run -p 2586:80 binwiederhier/ntfy serve`, then use `http://<address>:2586`.
- **ntfy.sh** (the public server): works anywhere, but the message text passes through a third party, and the topic
  name is the only protection: use a long random one. (It is why the example data in the tests never points there.)

What is sent: an alarm raised or cleared (with the value and the limit, in °C, % and hPa), a device that stops
sending or comes back, and the end of an OTA update. `cloud/dev/test-alerts.sh` checks the chain against a real
ntfy in Docker. Secrets (token, password) are stored in the database and never shown again.

## Limits to know

- **OTA.** One update per device at a time. Cancel only stops the chunks; the device gives up by its own timeout,
  because the protocol has no abort message. Images are not checked here: the device verifies the signature, and a
  bad image shows up as a `failed` result.
- **Login is on by default**, except in `dev/run-local.sh` (localhost only). Accounts are made on the machine with
  `python -m iotcloud.admin` (`iotcloud-admin` on the Pi); there is no sign-up and no password reset over HTTP.
  Sessions last 7 days; a password change ends them. The API does not rate-limit anything but logins.
- **Device logs.** The firmware publishes no log messages today (logs leave only on the serial console), so the
  "logs" you can download are what the cloud holds: readings, health, alarms, command results and OTA results.
  Real device logs need a firmware feature (a log topic fed by the logger sink, buffered like the other messages);
  it is a candidate for the roadmap, not a given.
- **The Raspberry Pi has 1 GB of RAM.** Everything here is a small process under systemd; no Docker, no Java.
- **The dev stack uses a plain local broker.** Mutual TLS and the access list are in `broker/`, tested there; the
  simulator and backend can use them with `--cafile/--cert/--key` and the `IOTCLOUD_MQTT_*` settings.
