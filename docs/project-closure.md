# Project closure

**Date:** 2026-11-14; **declared closed by the owner on 2026-11-15** (release `v1.0.0`). **Repository:** `main` (first commit 2026-03-31). **Result:** the V-model is complete from
requirements to validation, and the findings of the security review are fixed except the items listed in section 5, which the owner left open on purpose.

The system is an IoT environmental monitor of two nodes: a **Field Device** (STM32F469 Discovery: sensors, touch LCD, Modbus RTU slave) and a **Gateway**
(B-L475E-IOT01A: Modbus RTU master, MQTT over mutual TLS), with a **cloud service** and **broker** that stand in for AWS IoT Core. See the
[README](../README.md) for the architecture and [vision.md](vision.md) for the intent.

## 1. Where each phase ended

| Phase | Outcome | Record |
|---|---|---|
| Requirements | 207 requirements in the SRS, each traced to the design and to a test or a justification | [SRS.md](SRS.md), [requirements-traceability.md](test/requirements-traceability.md) |
| High-level design | Baseline `hld-v1.0`, amended since | [hld/](hld/) |
| Low-level design | Baseline `lld-v1.0`, amended where the boards or the validation showed a better rule (CP-D41, US-D8, the I2C recovery, the status pill) | [lld/](lld/) |
| Implementation | Closed 2026-11-08 (tag `phase-4-implementation-complete`): all modules of both boards, production `main()` on each | [implementation-phase-closure.md](implementation-phase-closure.md) |
| Verification | Closed 2026-11-14 (tag `phase-4-verification-complete`): 100 of 100 test cases pass; of the 207 requirements 182 are verified by test, 19 by hand, 5 waived by the owner, 1 not applicable, 0 failed; 16 defects found and closed | [verification-results.md](test/verification-results.md), [defects.md](test/defects.md) |
| Validation | Closed 2026-11-14: all 20 use cases run as their actor would run them on the real boards; 14 findings, all fixed | [validation-summary.md](test/validation-summary.md), [validation-plan.md](test/validation-plan.md) |

## 2. The system as delivered

- **Field Device:** samples temperature, humidity and pressure (and motion and magnetic values), evaluates alarms with hysteresis, shows readings, trend, status, alarms and
  configuration on the touch LCD, answers the Gateway over Modbus RTU, and has a console for diagnostics and provisioning.
- **Gateway:** polls the Field Device, publishes telemetry (QoS 0), health (QoS 0) and alarms (QoS 1) over MQTT with mutual TLS, buffers messages in flash while the
  connection is lost, accepts configuration and control commands from the cloud, and updates itself over the air with signed images in two flash banks, a
  trial boot, a self-check and an automatic rollback.
- **Cloud service and broker:** a service on a Raspberry Pi (API, web app, alert channels) behind a Mosquitto broker with certificate-based access control, with its own
  certificate authority. It plays the role AWS IoT Core has in the architecture.
- **State at closure:** Gateway firmware 1.1.22 (the first with downgrade protection), Field Device `v0.0.0+66f7b136`, cloud service and broker deployed from `main`; the Raspberry Pi is
  hardened (firewall, key-only SSH, updated packages); release tag `v1.0.0`.

## 3. The evidence

| Measure | Value |
|---|---|
| Host tests, Gateway | 1664 tested, 1661 passed, 0 failed, 3 ignored |
| Host tests, Field Device and shared | 903 tested, 898 passed, 0 failed, 5 ignored |
| Cloud and broker tests | 59 passed |
| Continuous integration | 15 checks (builds of both boards, host tests, clang-format, cppcheck, Markdown lint, cloud tests, installers); green on `main` |
| Verification | 100 Robot Framework test cases, 25 merged runs |
| Validation | Robot Framework and Playwright suites in [`validation/`](../validation/), records V-04 to V-33 |

## 4. What each level found

Each level found defects the one before it could not.

- **Unit and integration tests** gave the host results above and the first hardware bring-ups.
- **Verification** (against the requirements) found 16 defects (DEF-000 to DEF-015), for example a display that did not boot after a cold power-up, a Gateway that
  republished delivered messages after every reset, and a retry count configured as 1 instead of 3.
- **Validation** (against the use cases) found 14 findings (F-1 to F-14). F-9 to F-14 appeared only when a link was cut or a real update was run: telemetry lost
  while the cloud was catching up after a broker restart (QoS 0 is discarded for an offline session), a command that ran minutes late, a Gateway that
  could stay offline after an outage, and an interrupted firmware download that was abandoned because a dead link looks connected until the keep-alive notices.

## 5. What is not closed

The project is closed with these items open on purpose. None blocks the use of the system; each needs a decision or a resource the owner has not committed.

- **Security review items still open** ([security-review.md](security-review.md), section 6): **S-1**, the Gateway's private key is obfuscated and not protected while read-out protection is off (turning it on
  makes flashing harder); **S-5**, the release signing key has no passphrase yet (the tools support one). Everything else in the review is fixed or accepted.

- **Real AWS IoT Core.** Every cloud result is against the local stand-in. A connection to the real service is a self-contained job for when an AWS account is
  available: repeat the cloud-facing scenarios of `validation/suites/` against it (telemetry, health, alarms, commands, firmware update). Two behaviours depend on
  the broker and should be re-checked there: the QoS 0 queueing that finding F-9 needed, and the command expiry of finding F-10.
- **F-11, a Gateway that stayed offline after a broker outage.** The recovery now works whatever the cause, but the cause was never seen and the original failure did
  not recur in about 26 outages. Accepted as a residual risk; reopen it if a socket open keeps failing for more than three minutes.
- **Five requirements waived by the owner** ([not-run-closure.md](test/not-run-closure.md)) and **two use-case exceptions** that cannot be provoked safely on the system
  (a flash failure while deleting an image, a read error while checking a signature); both exceptions are covered by unit tests.
- **Known design limit:** the Gateway had little spare RAM at the implementation closure (see that report); check the margin before adding features.

## 6. Where to start

1. [README](../README.md): architecture, build and flash.
2. [use-case-descriptions.md](use-case-descriptions.md) and [SRS.md](SRS.md): what the system is for.
3. [test/validation-summary.md](test/validation-summary.md): what was proved and how to run it again.
4. [cloud/README.md](../cloud/README.md) and [cloud/DECISIONS.md](../cloud/DECISIONS.md): the cloud service and why it is built as it is.
5. Release and key handling for the Gateway images: [tools/ota/README.md](../tools/ota/README.md); the release key is kept outside the repository.
6. [security-review.md](security-review.md): the security review (no critical or high finding; findings S-1 to S-14 with recommendations).
