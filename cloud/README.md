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
| OTA service: firmware store, jobs, chunk server, progress and result | done, 21 tests in all |
| `web/` web app (Svelte): devices, live values, charts, alarms, commands, firmware update with progress, downloads | done |
| Alerts to the phone, login, TLS and per-device permissions, Pi deployment | later |

## Layout

```text
cloud/
  backend/    Python package `iotcloud`: ingest, database, API (FastAPI). Tests in backend/tests.
  sim/        the simulated device
  dev/        run the whole thing on this machine
  web/        the web app (to come)
  broker/     Mosquitto configuration for mutual TLS and per-device permissions (to come)
  deploy/     systemd units and Caddy configuration for the Raspberry Pi (to come)
```

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
| SNS | notifications to the phone (later) |
| API Gateway, Lambda | the FastAPI app |
| Cognito | one login on the web app (later) |
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

## Limits to know

- **OTA.** One update per device at a time. Cancel only stops the chunks; the device gives up by its own timeout,
  because the protocol has no abort message. Images are not checked here: the device verifies the signature, and a
  bad image shows up as a `failed` result.
- **No login yet.** Anyone who can reach the API can send commands and start an update. Keep it on localhost or the
  home network until the login and the broker's per-device permissions are in.
- **Device logs.** The firmware publishes no log messages today (logs leave only on the serial console), so the
  "logs" you can download are what the cloud holds: readings, health, alarms, command results and OTA results.
  Real device logs need a firmware feature (a log topic fed by the logger sink, buffered like the other messages);
  it is a candidate for the roadmap, not a given.
- **The Raspberry Pi has 1 GB of RAM.** Everything here is a small process under systemd; no Docker, no Java.
- **One broker for now.** The simulator and the backend use a plain local broker; mutual TLS comes with `broker/`.
