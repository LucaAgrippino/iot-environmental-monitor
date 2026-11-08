#!/usr/bin/env python3
"""Generate the verification method table from the SRS.

Reads every requirement of docs/SRS.md, joins it with the verification decision recorded in the SPEC
table below (one line per requirement) and writes

  docs/test/verification-method-table.md   the table for review and the plan's statistics
  docs/test/verification-methods.csv       the same data for the test runner and the report

The decisions are an engineering judgement, made from the requirement wording and not from the code. Edit
SPEC, rerun, review the diff. The script fails when a requirement has no decision or a decision names an
unknown requirement, so the table can never fall behind the SRS.

Method:  T test, I inspection, A analysis, D demonstration (the four classic verification methods).
Level:   U unit (host test, credited as evidence), I integration (modules or boards together),
         S system (the requirement on the real system).
Board:   GW Gateway, FD Field Device, BOTH each board, SYS the two boards and the cloud together.
Auto:    A fully automated, S semi-automatic (an operator action is needed), M manual, R review or tool run.
Expect:  '-' no known issue, KG known gap against the requirement (expected to fail), RK risk to watch.

Run from the repository root: python3 -I tools/gen-verification-methods.py
"""
import csv
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ID | method | level | board | auto | how it is verified | expectation (- | KG: .. | RK: ..)
SPEC = """
SA-000|T|S|FD|A|set the polling interval, reset, the console shows it|-
SA-010|T|S|FD|A|erase the ConfigStore, boot: the interval is 1 s|-
SA-020|T|U|BOTH|A|host tests: default ranges when the configuration read fails|-
SA-030|T|S|FD|A|boot log: temperature, humidity and pressure initialised|RK: LCD-O8, are the Field Device sensors real or simulated
SA-031|T|S|GW|A|boot log: all six sensor channels initialised|-
SA-040|T|U|BOTH|A|host test: a failing driver init logs an error|-
SA-050|T|U|BOTH|A|host tests: filter defaults when the configuration read fails|-
SA-060|T|U|BOTH|A|host tests: one failed sensor, the others keep running|-
SA-070|T|S|FD|A|console `sensors` updates at the configured interval|-
SA-071|T|S|GW|A|console shows six channels updating at the configured interval|-
SA-080|T|U|BOTH|A|host test: a read failure logs the error code|-
SA-090|T|U|BOTH|A|host tests: last validated value and the filter state are held|-
SA-100|T|S|BOTH|A|timestamp present in the snapshot, the register map and the telemetry payload|-
SA-110|T|S|GW|A|a periodic and a remote-read value pass the same filter|-
SA-120|T|U|BOTH|A|host tests: range validation|-
SA-130|T|U|BOTH|A|host tests: clamping to the range boundary|-
SA-140|T|U|BOTH|A|host tests: first-order IIR, alpha 0.1, step response|-
SA-150|T|I|FD|A|the register map value equals the value on the LCD|-
SA-160|T|S|GW|A|telemetry of an invalid channel carries the error flag|RK: needs a forced sensor fault
SA-170|T|S|GW|A|control `read` over MQTT triggers an extra read and a result|-
SA-175|T|U|BOTH|A|host tests: a failed read marks the reading invalid|-
SA-180|T|U|FD|A|host tests: rolling history of validated readings per channel|-
SA-190|T|U|FD|A|host tests: window of 5 minutes|-
SA-200|T|U|FD|A|host tests: fixed 5 s sampling independent of the polling rate|-
SA-210|T|U|FD|A|host tests: 60 entries per channel|-
SA-220|T|U|FD|A|host tests: value, validity flag and timestamp per entry|-
SA-230|T|U|FD|A|host tests and inspection: RAM only, cleared by reset|-
SA-240|T|U|FD|A|host tests: a stale reading is recorded invalid|-
AM-000|T|S|BOTH|A|set a threshold below the value: the alarm is raised|-
AM-010|T|S|BOTH|A|restore the threshold: the alarm clears|-
AM-011|T|U|BOTH|A|host tests: hysteresis on clear|-
AM-020|T|S|BOTH|A|an alarm notification is produced for an out-of-range value|-
AM-030|T|S|SYS|A|the alarm event reaches the broker|-
AM-040|T|S|GW|A|payload fields: sensor, type, value, threshold, timestamp|-
LD-000|D|S|FD|S|tap through the five screens|-
LD-010|T|S|FD|A|framebuffer of the Sensors screen against the golden image|-
LD-020|T|S|FD|A|the header clock equals the reading time (framebuffer)|-
LD-030|T|S|FD|A|units visible on the Sensors screen (framebuffer)|-
LD-040|T|S|FD|S|an invalid sensor shows the error state|RK: needs a forced sensor fault
LD-050|T|S|FD|A|values differ between two framebuffer captures|-
LD-060|T|S|FD|A|"waiting for data" visible right after boot|RK: timing
LD-070|T|S|FD|A|Status screen lists the resources (framebuffer)|-
LD-080|T|S|FD|A|raise an alarm: the Alarms screen lists it with its time|-
LD-090|T|S|FD|A|no alarm: the empty-state message|-
LD-100|T|S|FD|A|Config screen lists the polling rate and the thresholds|-
LD-110|T|S|FD|S|an out-of-range entry shows an error and blocks APPLY|-
LD-120|D|S|FD|S|APPLY opens the confirmation|-
LD-130|T|S|FD|S|CONFIRM applies the change, `config list` shows it|-
LD-140|T|U|FD|A|host tests: previous values kept until all are applied|-
LD-150|T|S|FD|S|CONFIRM, reset, the value persists|-
LD-155|T|S|FD|S|CANCEL and the 30 s timeout discard the edits|-
LD-160|T|S|FD|A|Trend screen against the golden image|-
LD-170|T|S|FD|A|after 5 minutes the chart spans the window|-
LD-180|T|S|FD|A|the chart changes when a sample is added|-
LD-190|T|S|FD|S|an invalid sample is a gap in the chart|RK: needs a forced sensor fault
LD-195|T|S|FD|A|"accumulating" indicator right after boot|-
LD-200|T|S|FD|A|splash visible during boot and gone at Operational|RK: timing
LD-210|T|S|FD|A|the progress bar is on the splash (framebuffer)|-
LD-220|T|S|FD|A|the bar advances 10, 30, 55, 80, 100 per step|RK: timing
LD-230|T|S|FD|A|the step names are shown|RK: timing
LD-240|T|S|FD|A|the firmware version is shown|-
LD-250|T|S|FD|A|LED pin levels read over SWD in each state|-
LD-260|T|S|FD|A|green LED on in Operational|-
LD-270|T|S|FD|A|green LED off in a fault state|-
LD-280|T|S|FD|A|red LED on while an alarm is active|-
LI-000|T|S|BOTH|A|connect with the documented serial parameters|-
LI-010|T|S|BOTH|A|run each Appendix A command, expect an answer|-
LI-020|T|S|BOTH|A|the result of each diagnostic is displayed|-
LI-030|T|S|GW|A|the provisioning menu is reachable|-
LI-040|T|S|GW|A|`prov set wifi-ssid` and `wifi-pass`, then reset: the board uses them (negative and positive case)|-
LI-050|T|S|GW|A|`prov set mqtt-endpoint`, reset: the endpoint is used (a wrong one gives no connection)|-
LI-060|T|S|GW|A|provision the certificates over the console|-
LI-070|T|S|BOTH|A|`prov set modbus-addr` takes effect|-
LI-080|T|S|GW|A|`prov set modbus-baud` and `modbus-parity` take effect|-
LI-090|T|S|BOTH|A|bad format and out-of-range values are refused|-
LI-100|T|S|BOTH|A|a change asks for confirmation|-
LI-110|T|S|BOTH|A|a confirmed change is applied|-
LI-120|T|S|BOTH|A|the previous value stays until all new values are applied|-
LI-130|T|S|BOTH|A|`selftest` runs|-
LI-140|T|S|BOTH|A|the self-test reports pass or fail|-
LI-150|T|S|BOTH|A|`serial` prints the MCU serial number|-
LI-160|T|S|GW|A|the last self-test result survives a reset|-
LI-170|T|S|BOTH|A|a failing diagnostic prints an error|-
LI-180|T|S|BOTH|A|a rejected value prints the problem|-
LI-190|T|S|BOTH|A|no confirmation: the input is discarded|-
MB-000|T|S|FD|A|a register changes when a measurement is ready|-
MB-010|T|S|SYS|A|the Gateway reads the sensor registers of the Field Device|-
MB-020|T|S|SYS|A|the time write reaches the Field Device|-
MB-030|T|S|SYS|A|line parameters measured with the RS-485 probe|-
MB-040|T|I|SYS|A|function codes seen on the line with the probe|-
MB-050|T|S|SYS|A|no slave: the request times out after 200 ms|-
MB-060|T|S|SYS|A|a failed transaction is sent 4 times|-
MB-070|I|S|BOTH|R|the register map in the code equals modbus-register-map.md|-
MB-080|T|S|SYS|A|a remote command arrives in the holding registers|-
MB-090|T|S|SYS|A|the command result returns to the cloud|-
MB-100|T|U|GW|A|host tests: several slave addresses|-
MB-110|T|I|GW|A|`profiles add`, list, remove|-
MB-111|T|I|GW|A|a profile needs its mandatory fields|-
MB-120|T|S|SYS|A|a wrong device identifier is rejected at link establishment|-
MB-130|T|S|SYS|A|a profile mismatch is logged with identifier and address|-
MB-140|T|S|GW|A|a malformed remote command is rejected and logged|-
CC-000|T|S|SYS|A|telemetry on the broker at the configured period|-
CC-010|T|S|SYS|A|health payload fields|-
CC-020|T|S|SYS|A|the alarm notification is published|-
CC-030|T|S|SYS|A|change the telemetry interval by console and by cloud command|-
CC-040|T|S|SYS|A|change the health interval by console and by cloud command|-
CC-050|T|S|SYS|A|TLS 1.2 with a client certificate is accepted by the broker|-
CC-060|T|S|SYS|A|stop the broker, restart it, the device reconnects|-
CC-070|T|S|SYS|A|every payload parses as JSON|-
CC-071|T|S|SYS|A|every payload has a schema version|-
CC-080|T|S|SYS|A|telemetry, alarms and health use separate topics|-
CC-090|T|S|SYS|A|the serial number is in every payload|-
TS-000|T|S|GW|A|the time updates at boot and periodically|-
TS-010|T|S|GW|A|NTP synchronisation from the configured server list|-
TS-020|T|S|GW|A|the RTC keeps the time across a reset|-
TS-030|T|S|SYS|A|the Field Device clock receives the time on connection|-
TS-040|T|S|GW|A|without NTP the data is flagged unsynchronised|-
TS-050|T|S|GW|A|restore the network: the time update is retried|-
TS-060|T|S|SYS|A|unplug and replug RS-485: the time is written again|-
DM-000|T|S|SYS|A|a parameter change by cloud command is accepted|-
DM-001|T|S|SYS|A|an invalid parameter change is refused|-
DM-002|T|S|SYS|A|a confirmation or rejection is sent to the cloud|-
DM-010|T|S|SYS|A|the remote restart command restarts the device|-
DM-020|T|S|SYS|A|no confirmation: the restart is cancelled|-
DM-021|T|S|SYS|A|a declined confirmation cancels the restart|-
DM-030|T|S|SYS|A|restart success is reported after the self-check|-
DM-040|T|S|SYS|A|the self-check verifies sensors and links|-
DM-050|T|S|GW|A|the firmware downloads over MQTT/TLS|-
DM-051|T|S|GW|A|a download resumes at the interruption point|-
DM-052|T|S|GW|A|3 consecutive failures discard the partial image|-
DM-053|T|S|GW|A|a download failure logs an error|-
DM-054|T|S|GW|A|a second update command is refused while one runs|-
DM-055|T|S|GW|A|the update result is reported|-
DM-056|T|S|GW|A|an update command from a wrong source is refused|-
DM-060|T|S|GW|A|a bad size, format or header is rejected|-
DM-061|T|S|GW|A|a failed validation restores the current firmware|-
DM-062|T|S|GW|A|a failed validation logs an error|-
DM-070|T|S|GW|A|the inactive bank becomes the boot bank|-
DM-071|T|S|GW|A|the new firmware runs a self-check|-
DM-072|T|S|GW|A|a failed self-check rolls back|-
DM-073|T|S|GW|A|the current image is kept until the self-check passes|-
DM-074|I|S|GW|R|dual-bank partitions per the layout document|-
DM-080|T|S|GW|A|a wrong signature is refused|-
DM-090|T|S|BOTH|A|every change survives a reset|-
DM-100|T|S|GW|A|a profile update by console and by remote configuration|-
DM-101|T|S|GW|A|a changed profile re-probes the slave|-
BF-000|T|S|GW|A|cut the network: messages are buffered in flash|-
BF-010|T|S|GW|A|restore the network: delivered in order|-
BF-020|T|S|GW|A|more than 254 buffered entries: the oldest are dropped|-
NF-100|T|S|BOTH|A|worst polling cycle measured against 100 ms|-
NF-101|T|S|BOTH|A|alarm detected within one cycle|-
NF-102|T|S|BOTH|A|alarm cleared within one cycle|-
NF-103|T|S|SYS|A|3 failed transactions declare the link failed|-
NF-104|T|S|SYS|A|3 good responses declare it restored|-
NF-105|T|S|SYS|A|200 ms response timeout measured with the probe|-
NF-106|T|S|GW|A|a message is queued within 200 ms of the data|-
NF-107|T|S|GW|A|a remote command starts within 500 ms|-
NF-108|T|S|FD|A|LCD refresh rate measured|RK: UI task runs every 40 ms, the value refresh every 200 ms
NF-109|T|S|BOTH|A|hang the sensor task: watchdog reset after 10 s|-
NF-110|T|S|SYS|A|default polling frames at 1 Hz on the line|-
NF-111|T|S|SYS|A|telemetry period 60 s|-
NF-112|T|S|SYS|A|health period 5 min|-
NF-113|T|S|SYS|A|alarm latency at most 500 ms (the Raspberry Pi bench)|-
NF-114|T|S|BOTH|A|100 ms and 10 s accepted, outside refused|-
NF-200|T|S|GW|A|network off and broker unreachable: sensors and alarms keep running, no reset loop|-
NF-201|T|S|SYS|A|a short bus interruption recovers without a restart|-
NF-202|T|S|BOTH|A|watchdog reset: Operational within 5 s|-
NF-203|T|S|BOTH|A|normal reset: Operational within 5 s|-
NF-204|T|S|GW|A|a failed update rolls back within 10 s|-
NF-205|T|U|BOTH|A|host tests: a failed sensor is reinitialised|-
NF-206|T|S|SYS|A|telemetry is published with QoS 0|-
NF-207|T|S|SYS|A|alarms are published with QoS 1|-
NF-208|T|U|BOTH|A|host tests: the error indicator value|-
NF-209|T|S|GW|A|reconnect after 2 s, doubling to 60 s|-
NF-210|T|S|GW|A|RTC resync period, configurable from the console|-
NF-211|T|S|SYS|A|the Field Device time is written after each NTP sync|-
NF-212|T|S|GW|A|covered by TS-040|-
NF-213|T|S|BOTH|A|power-on to Operational within 10 s|-
NF-214|T|S|GW|S|power cut during a flash write: the device recovers|RK: needs a controlled power cut
NF-215|T|S|SYS|A|3 missed polls: the node is reported offline|-
NF-216|T|S|SYS|A|health is published with QoS 0|-
NF-300|T|S|SYS|A|TLS 1.2 or higher on the wire|-
NF-301|T|S|SYS|A|the client certificate is required and checked|-
NF-302|I|S|GW|R|inspect storage: no plaintext passphrase or private key|-
NF-303|I|S|BOTH|R|no network console in the code, UART only|-
NF-304|T|S|GW|A|an image with a wrong signature is refused|-
NF-305|T|S|GW|A|a plain-TCP broker is refused|-
NF-306|T|S|SYS|A|Tier 1 parameters are refused over the cloud|-
NF-307|T|S|SYS|A|a restart without confirmation does nothing|-
NF-308|T|S|SYS|A|an acknowledgement follows a non-disruptive command|-
NF-400|A|S|GW|A|linker output: RAM within 128 KB|RK: 1 224 B spare
NF-401|A|S|GW|A|linker output: flash within 1 MB|-
NF-402|A|S|GW|A|QSPI map: all regions within 8 MB|-
NF-403|A|S|FD|A|linker output: RAM within 320 KB|-
NF-404|A|S|FD|A|linker output: flash within 2 MB|-
NF-405|A|S|FD|A|QSPI map: all regions within 16 MB|-
NF-406|A|S|GW|A|log partition size at most 3 MB|-
NF-407|A|S|GW|A|buffer partition size at most 4 MB|-
NF-408|A|S|BOTH|A|no malloc or free call reachable in the image (symbol check)|-
NF-500|T|S|BOTH|A|log lines carry level, time and module|-
NF-501|I|S|FD|A|the HAL check script and the include rule|-
NF-502|I|S|BOTH|R|clang-format and cppcheck in CI plus a BARR-C review|RK: no full review done yet
NF-503|T|S|BOTH|A|the version via the console and in the health message|-
NF-504|D|S|BOTH|A|trace output on the debug channel|-
NF-505|I|S|BOTH|R|run Doxygen, read the warnings|RK: never run
NF-506|I|S|BOTH|R|review of the interfaces plus the HAL check|-
NF-507|T|S|BOTH|A|fault injection: the cause is reported at the next start|-
"""


def load_srs():
    body = (ROOT / "docs/SRS.md").read_text().split("## 6. Traceability")[0]
    out = {}
    for m in re.finditer(r"^- \[(REQ-[A-Z]+-\d+)\]\s*(.*)", body, re.M):
        out[m.group(1)] = re.sub(r"\s+", " ", m.group(2)).strip()
    return out


def load_spec():
    rows = {}
    for line in SPEC.strip().split("\n"):
        f = line.split("|")
        if len(f) != 7:
            sys.exit("bad spec line: " + line)
        rid = "REQ-" + f[0]
        if rid in rows:
            sys.exit("duplicate spec for " + rid)
        rows[rid] = dict(zip(["id", "method", "level", "board", "auto", "means", "expect"], [rid] + f[1:]))
        if f[1] not in "TIAD" or f[2] not in "UIS" or f[4] not in "ASMR":
            sys.exit("bad code in: " + line)
    return rows


def main():
    req = load_srs()
    spec = load_spec()
    missing = sorted(set(req) - set(spec))
    unknown = sorted(set(spec) - set(req))
    if missing or unknown:
        sys.exit("SRS and SPEC differ. Missing decisions: %s. Unknown IDs: %s" % (missing, unknown))

    rows = []
    for rid in req:
        r = dict(spec[rid])
        r["text"] = re.sub(r"^The system shall ", "", req[rid])
        rows.append(r)

    csv_path = ROOT / "docs/test/verification-methods.csv"
    with csv_path.open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["id", "method", "level", "board", "automation", "means", "expectation", "requirement"])
        for r in rows:
            w.writerow([r["id"], r["method"], r["level"], r["board"], r["auto"], r["means"], r["expect"], req[r["id"]]])

    by = lambda k: Counter(r[k] for r in rows)
    names = {"T": "Test", "I": "Inspection", "A": "Analysis", "D": "Demonstration"}
    lv = {"U": "Unit (host test, credited)", "I": "Integration", "S": "System"}
    au = {"A": "Automated", "S": "Semi-automatic", "M": "Manual", "R": "Review or tool run"}
    kg = [r for r in rows if r["expect"].startswith("KG")]
    rk = [r for r in rows if r["expect"].startswith("RK")]

    md = []
    md.append("# Verification method table\n")
    md.append("**Generated by `tools/gen-verification-methods.py` from `docs/SRS.md` and the decisions in that script. "
              "Do not edit by hand: change the SPEC table in the script and rerun.**\n")
    md.append("Requirements: **%d**. Method codes: T test, I inspection, A analysis, D demonstration. Level: U unit "
              "(existing host tests, credited as evidence and re-run at the baseline), I integration, S system. "
              "Board: GW Gateway, FD Field Device, BOTH each board, SYS the two boards and the broker together. "
              "Automation: A automated, S semi-automatic (an operator action), M manual, R review or tool run. "
              "Expectation: KG known gap, RK risk to watch. See `verification-plan.md`.\n" % len(rows))
    md.append("## Statistics\n")
    md.append("| Method | Count |\n|---|---|")
    for k in "TIAD":
        md.append("| %s | %d |" % (names[k], by("method")[k]))
    md.append("\n| Level | Count |\n|---|---|")
    for k in "USI":
        md.append("| %s | %d |" % (lv[k], by("level")[k]))
    md.append("\n| Automation | Count |\n|---|---|")
    for k in "ASMR":
        md.append("| %s | %d |" % (au[k], by("auto")[k]))
    md.append("\n| Board | Count |\n|---|---|")
    for k in ["GW", "FD", "BOTH", "SYS"]:
        md.append("| %s | %d |" % (k, by("board")[k]))
    md.append("\n## Known gaps (expected to fail)\n")
    md.append("| ID | Requirement | Gap |\n|---|---|---|")
    for r in kg:
        md.append("| %s | %s | %s |" % (r["id"], r["text"][:80], r["expect"][4:]))
    md.append("\n## Risks to watch\n")
    md.append("| ID | Risk |\n|---|---|")
    for r in rk:
        md.append("| %s | %s |" % (r["id"], r["expect"][4:]))
    md.append("\n## Table\n")
    md.append("| ID | Requirement | M | L | Board | Auto | How it is verified | Expectation |\n|---|---|---|---|---|---|---|---|")
    for r in rows:
        md.append("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["id"], r["text"][:70].replace("|", "/"), r["method"], r["level"], r["board"], r["auto"], r["means"], r["expect"]))
    (ROOT / "docs/test/verification-method-table.md").write_text("\n".join(md) + "\n")
    print("%d requirements: %s | KG %d | RK %d" % (len(rows), dict(by("method")), len(kg), len(rk)))


if __name__ == "__main__":
    main()
