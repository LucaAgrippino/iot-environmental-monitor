#!/usr/bin/env python3
"""Draw every diagram of the cloud guide into diagrams/*.svg.

Structure diagrams are Graphviz (`dot`); message-sequence diagrams are drawn here, because Graphviz has no notion of
time. Run it through build.sh (it needs `dot` on the PATH).
"""
import html
import subprocess
import sys
from pathlib import Path

OUT = Path(__file__).parent / "diagrams"
OUT.mkdir(exist_ok=True)

FONT = "Liberation Sans"
MONO = "Liberation Mono"
C = {  # fill, stroke
    "dev": ("#E3F0FF", "#2F6DB5"),
    "brk": ("#E4F6E8", "#2E8B57"),
    "be": ("#EEE8FA", "#6B4FB3"),
    "web": ("#FFF1DD", "#C77B12"),
    "oth": ("#F3F3F3", "#7A7A7A"),
    "sec": ("#FDE8E8", "#C0392B"),
}


def node(name, label, kind="oth", shape="box", extra=""):
    f, s = C[kind]
    return (f'  {name} [label=<{label}> shape={shape} style="rounded,filled" fillcolor="{f}" color="{s}" '
            f'penwidth=1.4 {extra}];\n')


def cluster(name, label, kind, body):
    f, s = C[kind]
    return (f'  subgraph cluster_{name} {{ label=<<B>{label}</B>> style="rounded,filled" fillcolor="{f}33" color="{s}" '
            f'fontcolor="{s}" fontsize=12 margin=14;\n{body}  }}\n')


def dot(name, src, rankdir="LR", extra=""):
    head = (f'digraph G {{ rankdir={rankdir}; bgcolor="transparent"; pad=0.2; nodesep=0.35; ranksep=0.55; {extra}\n'
            f'  node [fontname="{FONT}" fontsize=11 margin="0.14,0.08"];\n'
            f'  edge [fontname="{FONT}" fontsize=9.5 color="#555555" arrowsize=0.8];\n')
    p = OUT / f"{name}.dot"
    p.write_text(head + src + "}\n")
    subprocess.run(["dot", "-Tsvg", str(p), "-o", str(OUT / f"{name}.svg")], check=True)
    p.unlink()


# --------------------------------------------------------------------------------------------------------------------
# 1. The big picture
# --------------------------------------------------------------------------------------------------------------------
dev = (node("fd", "<B>Field Device</B><BR/>STM32F469 Discovery<BR/>touch display, Modbus slave<BR/>"
            "<I>simulated sensors</I>", "dev") +
       node("gw", "<B>Gateway</B><BR/>B-L475E-IOT01A<BR/>real sensors, Wi-Fi module<BR/>"
            "Modbus master, cloud bridge", "dev"))
pi = (node("br", "<B>Broker</B><BR/>Mosquitto :8883<BR/>mutual TLS + access list", "brk") +
      node("be", "<B>Backend</B>&#160;(iotcloud)<BR/>stores, serves, commands, alerts", "be") +
      node("db", "<B>Database</B><BR/>SQLite + firmware files", "be", "cylinder") +
      node("cd", "<B>Caddy</B> :443<BR/>HTTPS front door", "web"))
ppl = (node("br2", "<B>Browser</B><BR/>the web app", "web") +
       node("ph", "<B>Phone</B><BR/>ntfy push", "oth") +
       node("op", "<B>Operator PC</B><BR/>CA, signing key,<BR/>firmware builds", "sec"))
dot("overview",
    cluster("dev", "Devices (the bench)", "dev", dev + "    {rank=same; fd; gw;}\n") +
    cluster("pi", "Raspberry Pi 3B (the cloud)", "be", pi + "    {rank=same; br; be;}\n    {rank=same; db; cd;}\n") +
    node("br2", "<B>Browser</B><BR/>the web app", "web") + node("ph", "<B>Phone</B><BR/>ntfy push", "oth") +
    node("op", "<B>Operator PC</B><BR/>CA, signing key,<BR/>firmware builds", "sec") +
    "  {rank=same; op; br2; ph;}\n" +
    '  fd -> gw [label="RS-485\\nModbus RTU" dir=both];\n'
    '  gw -> br [label="Wi-Fi, MQTT over TLS\\nup: telemetry, alarms, health\\ndown: commands, firmware" dir=both color="#2F6DB5" penwidth=1.8];\n'
    '  br -> be [label="subscribe /\\npublish" dir=both];\n'
    '  be -> db [dir=both]; be -> cd [label="http, localhost:8000" dir=both];\n'
    '  cd -> br2 [label="https" dir=both]; be -> ph [label="alerts" style=dashed];\n'
    '  op -> br [label="signed firmware\\n(OTA tool, optional)" style=dashed color="#C0392B"];\n',
    rankdir="TB", extra="nodesep=0.5; ranksep=0.55;")

# --------------------------------------------------------------------------------------------------------------------
# 2. Where things live in the repository
# --------------------------------------------------------------------------------------------------------------------
rep = (
    node("cloud", "<B>cloud/</B><BR/>this guide's subject", "be") +
    node("backend", "<B>backend/</B><BR/>Python package <I>iotcloud</I><BR/>ingest, API, OTA, alerts, login", "be") +
    node("web", "<B>web/</B><BR/>Svelte source of the web app<BR/><I>dist/</I> = built files", "web") +
    node("deploy", "<B>deploy/</B><BR/>bundle, installer,<BR/>systemd unit, Caddyfile", "brk") +
    node("sim", "<B>sim/</B><BR/>a fake device for testing", "dev") +
    node("dev", "<B>dev/</B><BR/>run everything on the PC", "oth") +
    node("doc", "<B>doc/</B><BR/>this guide", "oth") +
    node("broker", "<B>broker/</B> (top level)<BR/>own CA, Mosquitto config,<BR/>access list, installer", "sec") +
    node("tools", "<B>tools/ota/</B> + <B>scripts/</B><BR/>sign firmware, build releases,<BR/>provision devices, OTA tool", "sec") +
    node("fw", "<B>firmware/</B><BR/>the code on the two boards<BR/>(talks to the cloud)", "dev"))
dot("repo",
    rep +
    '  cloud -> backend; cloud -> web; cloud -> deploy; cloud -> sim; cloud -> dev; cloud -> doc;\n'
    '  web -> backend [label="built files served by" style=dashed constraint=false];\n'
    '  sim -> backend [label="same messages" style=dashed constraint=false];\n'
    '  fw -> backend [label="message contract (schema 1.1)" style=dashed color="#2F6DB5"];\n'
    '  broker -> backend [label="certificates" style=dashed color="#C0392B"];\n'
    '  tools -> fw [label="signs / provisions" style=dashed color="#C0392B"];\n',
    rankdir="LR", extra="nodesep=0.22; ranksep=0.9;")

# --------------------------------------------------------------------------------------------------------------------
# 3. Trust: who vouches for whom
# --------------------------------------------------------------------------------------------------------------------
# --------------------------------------------------------------------------------------------------------------------
# 4. Backend architecture
# --------------------------------------------------------------------------------------------------------------------
mq = (node("mqtt", "<B>mqtt.py</B><BR/>MQTT client thread<BR/>subscribes to dt/iotmonitor/+/…", "be") +
      node("ing", "<B>ingest.py</B><BR/>parse topic, validate JSON,<BR/>write rows", "be") +
      node("ota", "<B>ota.py</B><BR/>image store, jobs,<BR/>chunk server", "be") +
      node("cmd", "<B>commands.py</B><BR/>config / control commands<BR/>and their results", "be"))
ap = (node("api", "<B>api.py</B><BR/>FastAPI routes /api/…<BR/>+ serves the web app at /", "be") +
      node("auth", "<B>auth.py</B><BR/>users, sessions,<BR/>roles, lockout", "be") +
      node("hub", "<B>live.py</B><BR/>Hub: fans events out to<BR/>every open browser stream", "be"))
nt = (node("nq", "<B>notify.py</B>&#160;Notifier<BR/>queue + worker thread", "be") +
      node("nw", "Watcher thread<BR/>'device went quiet / is back'", "be"))
# --------------------------------------------------------------------------------------------------------------------
# 5. Front-end architecture
# --------------------------------------------------------------------------------------------------------------------
tabs = "".join(node(f"t{i}", f"<B>{n}</B><BR/>{d}", "web") for i, (n, d) in enumerate([
    ("Overview", "latest values,<BR/>active alarms, health"), ("Charts", "history: both boards"),
    ("Alarms", "alarm history"), ("Commands", "read, restart, config,<BR/>result of each"),
    ("Firmware", "upload images, start OTA,<BR/>progress bar"), ("Downloads", "CSV / JSON of<BR/>readings, health, alarms")]))
dot("frontend",
    node("html", "<B>index.html</B> + <B>main.js</B><BR/>the page the browser loads", "web") +
    node("app", "<B>App.svelte</B><BR/>side bar: devices + Alerts<BR/>decides what to show", "web") +
    node("login", "<B>Login.svelte</B>", "web") + node("alerts", "<B>Alerts.svelte</B><BR/>channels, test, send log", "web") +
    node("dv", "<B>DeviceView.svelte</B><BR/>header + tab bar", "web") +
    cluster("tabs", "src/tabs/", "web", tabs) +
    cluster("lib", "src/lib/ (shared helpers)", "oth",
            node("api", "<B>api.js</B><BR/>fetch wrapper for /api", "oth") +
            node("live", "<B>live.svelte.js</B><BR/>one EventSource,<BR/>counter that ticks per event", "oth") +
            node("auth", "<B>auth.svelte.js</B><BR/>who is logged in", "oth") +
            node("chart", "<B>Chart.svelte</B><BR/>small SVG line chart", "oth")) +
    node("be", "<B>Backend</B> /api/* and /api/stream", "be") +
    '  html -> app; app -> login [label="not signed in"]; app -> alerts; app -> dv; dv -> t0; dv -> t1; dv -> t2;\n'
    '  dv -> t3; dv -> t4; dv -> t5; t1 -> chart [style=dashed];\n'
    '  app -> auth [style=dashed]; dv -> api [style=dashed]; live -> app [label="refresh on event" style=dashed constraint=false];\n'
    '  api -> be [label="GET / POST / PUT / DELETE" dir=both]; live -> be [label="server-sent events" dir=back];\n',
    rankdir="TB", extra="ranksep=0.45; nodesep=0.25;")

# --------------------------------------------------------------------------------------------------------------------
# 6. Database
# --------------------------------------------------------------------------------------------------------------------
# --------------------------------------------------------------------------------------------------------------------
# 7. Setup flow (from nothing to a device on the screen)
# --------------------------------------------------------------------------------------------------------------------
def step(n, title, sub, kind):
    return node(f"s{n}", f"<B>{n}. {title}</B><BR/>{sub}", kind)


dot("setup",
    step(1, "Make the CA", "iotpki.py init", "sec") +
    step(2, "Issue certificates", "broker, backend,<BR/>operator, device", "sec") +
    step(3, "Install the broker", "install-broker.sh<BR/>on the Pi", "brk") +
    step(4, "Install the cloud", "build-bundle.sh,<BR/>install.sh on the Pi", "be") +
    step(5, "Create a login", "iotcloud-admin<BR/>add-user", "web") +
    step(6, "Give the backend<BR/>its certificate", "/etc/iotcloud/certs<BR/>+ env file", "be") +
    step(7, "Provision the device", "provision_device.py:<BR/>CA, certificate, key,<BR/>endpoint", "dev") +
    step(8, "Reset the device", "it reads its<BR/>identity only at boot", "dev") +
    step(9, "Check", "broker log, then<BR/>the web app", "web") +
    "  s1 -> s2 -> s3 -> s4 -> s5 -> s6 -> s7 -> s8 -> s9;\n",
    rankdir="TB", extra="ranksep=0.28; nodesep=0.3;")

# --------------------------------------------------------------------------------------------------------------------
# 8. OTA pipeline overview
# --------------------------------------------------------------------------------------------------------------------
ops = (step(1, "Link twice", "once for Slot A,<BR/>once for Slot B", "sec") +
       step(2, "Sign both", "make_image.py sign<BR/>(ECDSA P-256 header)", "sec") +
       step(3, "Upload", "web app: Firmware tab<BR/>(two .ota files)", "web") +
       step(4, "Start the update", "backend publishes<BR/>cmd/…/ota", "be"))
dv = (step(5, "Download", "chunk by chunk into<BR/>QSPI staging", "dev") +
      step(6, "Verify", "SHA-256 + signature<BR/>against built-in key", "dev") +
      step(7, "Install", "copy into the bank<BR/>it is not running from", "dev") +
      step(8, "Reboot into it", "bootloader tries the<BR/>new image (trial)", "dev") +
      step(9, "Self-check", "sensors, Modbus,<BR/>cloud connection", "dev") +
      node("ok", "<B>Confirmed</B><BR/>report <I>success</I>", "brk") +
      node("rb", "<B>Rolled back</B><BR/>old image runs,<BR/>report <I>rolled_back</I>", "sec"))
dot("ota_pipeline",
    cluster("op", "On the operator's PC and the cloud", "web", ops) +
    cluster("dv", "On the Gateway", "dev", dv) +
    "  s1 -> s2 -> s3 -> s4; s4 -> s5 [constraint=false label=\"cmd/…/ota\" style=dashed]; s5 -> s6 -> s7 -> s8 -> s9; s9 -> ok [label=\"pass\" color=\"#2E8B57\"];\n"
    "  s9 -> rb [label=\"fail or crash\" color=\"#C0392B\"];\n"
    "  s6 -> rb [label=\"bad signature:\\nstop, report failed\" style=dashed color=\"#C0392B\" constraint=false];\n",
    rankdir="TB", extra="newrank=true; ranksep=0.28; nodesep=0.35;")

# --------------------------------------------------------------------------------------------------------------------
# 9. Flash layout
# --------------------------------------------------------------------------------------------------------------------
def region(addr, size, label, f, s):
    return (f'<TR><TD ALIGN="LEFT" BGCOLOR="#FFFFFF"><FONT FACE="{MONO}" POINT-SIZE="9">{addr}</FONT></TD>'
            f'<TD BGCOLOR="{f}" COLOR="{s}" WIDTH="230" ALIGN="LEFT"><B>{label}</B><BR/><FONT POINT-SIZE="9">{size}</FONT></TD></TR>')


onchip = "".join([
    region("0x0800_0000", "16 KB", "Bootloader", "#FDE8E8", "#C0392B"),
    region("0x0800_4000", "8 KB", "Boot records A/B + slot descriptors", "#FFF1DD", "#C77B12"),
    region("0x0800_6000", "480 KB  (hardware bank 1)", "Slot A: firmware image", "#E3F0FF", "#2F6DB5"),
    region("0x0808_0000", "480 KB  (hardware bank 2)", "Slot B: firmware image", "#E4F6E8", "#2E8B57")])
qspi = "".join([
    region("0x9000_0000", "64 KB", "ConfigStore (settings)", "#F3F3F3", "#7A7A7A"),
    region("0x9001_0000", "64 KB", "CertStore: CA, certificate, key, Wi-Fi", "#FDE8E8", "#C0392B"),
    region("0x9002_0000", "1 MB", "Log ring + store-and-forward buffer", "#EEE8FA", "#6B4FB3"),
    region("0x9012_0000", "4 MB", "OTA staging: the download lands here", "#FFF1DD", "#C77B12")])
dot("flash",
    f'  a [shape=plain label=<<TABLE BORDER="0" CELLBORDER="1" CELLSPACING="2" CELLPADDING="5">'
    f'<TR><TD COLSPAN="2" BORDER="0"><B>Inside the STM32L475 (1 MB)</B></TD></TR>{onchip}</TABLE>>];\n'
    f'  b [shape=plain label=<<TABLE BORDER="0" CELLBORDER="1" CELLSPACING="2" CELLPADDING="5">'
    f'<TR><TD COLSPAN="2" BORDER="0"><B>External QSPI flash (8 MB)</B></TD></TR>{qspi}</TABLE>>];\n'
    '  b -> a [label="verified image is\\ncopied here, into the\\nbank NOT running" color="#C77B12" penwidth=1.6];\n',
    rankdir="LR")

# --------------------------------------------------------------------------------------------------------------------
# 10. Boot state machine
# --------------------------------------------------------------------------------------------------------------------
def st(n, label, kind):
    return node(n, label, kind, "box", 'width=1.7')


dot("bootstate",
    st("c", "<B>CONFIRMED</B><BR/>normal operation", "brk") +
    st("p", "<B>TRIAL_PENDING</B><BR/>new image armed", "web") +
    st("r", "<B>TRIAL_RUNNING</B><BR/>new image booted,<BR/>not yet trusted", "be") +
    st("b", "<B>ROLLED_BACK</B><BR/>old image restored", "sec") +
    '  c -> p [label="app: activate\\n(after install)"];\n'
    '  p -> r [label="bootloader: boots\\nthe new bank"];\n'
    '  r -> c [label="app: self-check passed,\\nconfirm" color="#2E8B57"];\n'
    '  r -> b [label="self-check failed, or crash,\\nwatchdog, power loss" color="#C0392B"];\n'
    '  b -> c [label="app: reports rolled_back,\\nacknowledges"];\n',
    rankdir="TB", extra="ranksep=0.5; nodesep=0.9;")

# --------------------------------------------------------------------------------------------------------------------
# Sequence diagrams (drawn here)
# --------------------------------------------------------------------------------------------------------------------
SEQ_COL = {"dev": "#2F6DB5", "brk": "#2E8B57", "be": "#6B4FB3", "web": "#C77B12", "oth": "#7A7A7A", "sec": "#C0392B"}


def seq(name, parts, steps, width=980, gap=30):
    """parts: [(id, label, kind)]; steps: ('msg', a, b, text, dashed) | ('note', a, text) | ('sep', text)."""
    n = len(parts)
    left = 70
    colw = (width - 2 * left) / max(n - 1, 1)
    x = {p[0]: left + i * colw for i, p in enumerate(parts)}
    y0 = 70
    h = y0 + gap * len(steps) + 60
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {h}" font-family="{FONT}, Arial, sans-serif">',
         '<defs><marker id="ar" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
         '<path d="M0,0 L10,5 L0,10 z" fill="#444"/></marker></defs>']
    for pid, label, kind in parts:
        c = SEQ_COL[kind]
        lines = label.split("\n")
        o.append(f'<rect x="{x[pid] - 62}" y="8" width="124" height="{14 + 13 * len(lines)}" rx="7" fill="{c}22" stroke="{c}" '
                 f'stroke-width="1.4"/>')
        for i, ln in enumerate(lines):
            o.append(f'<text x="{x[pid]}" y="{26 + 13 * i}" text-anchor="middle" font-size="11" font-weight="{700 if i == 0 else 400}" '
                     f'fill="#222">{html.escape(ln)}</text>')
        o.append(f'<line x1="{x[pid]}" y1="{22 + 13 * len(lines)}" x2="{x[pid]}" y2="{h - 14}" stroke="{c}" stroke-width="1" '
                 f'stroke-dasharray="4 4" opacity=".7"/>')
    y = y0
    num = 0
    for s in steps:
        if s[0] == "msg":
            _, a, b, text, dashed = s
            num += 1
            xa, xb = x[a], x[b]
            if a == b:
                if xa > width / 2:  # right half: loop and text go to the left, so the text stays inside the picture
                    o.append(f'<path d="M{xa},{y} h-26 v14 h26" fill="none" stroke="#444" stroke-width="1.2" marker-end="url(#ar)"/>')
                    tx, anchor = xa - 32, "end"
                else:
                    o.append(f'<path d="M{xa},{y} h26 v14 h-26" fill="none" stroke="#444" stroke-width="1.2" marker-end="url(#ar)"/>')
                    tx, anchor = xa + 32, "start"
                ty = y + 4
            else:
                d = ' stroke-dasharray="5 3"' if dashed else ""
                o.append(f'<line x1="{xa}" y1="{y}" x2="{xb}" y2="{y}" stroke="#444" stroke-width="1.3"{d} marker-end="url(#ar)"/>')
                tx, anchor = (xa + xb) / 2, "middle"
                ty = y - 5
            o.append(f'<text x="{tx}" y="{ty}" text-anchor="{anchor}" font-size="11.5" fill="#111">'
                     f'<tspan font-weight="700" fill="#6B4FB3">{num}</tspan>  {html.escape(text)}</text>')
            y += gap + (14 if a == b else 0)
        elif s[0] == "note":
            _, a, text = s
            w = max(150, 6.6 * len(text))
            cx = min(max(x[a], 14 + w / 2), width - 14 - w / 2)  # keep the box inside the picture
            o.append(f'<rect x="{cx - w / 2}" y="{y - 12}" width="{w}" height="22" rx="4" fill="#FFFBE6" stroke="#D9C46B"/>')
            o.append(f'<text x="{cx}" y="{y + 3}" text-anchor="middle" font-size="11" fill="#333">{html.escape(text)}</text>')
            y += gap
        elif s[0] == "sep":
            o.append(f'<line x1="14" y1="{y - 6}" x2="{width - 14}" y2="{y - 6}" stroke="#999" stroke-dasharray="2 3"/>')
            o.append(f'<text x="{width / 2}" y="{y + 8}" text-anchor="middle" font-size="11.5" font-style="italic" fill="#555" '
                     f'style="paint-order:stroke" stroke="#fff" stroke-width="4">{html.escape(s[1])}</text>')
            y += gap + 4
    o.append("</svg>")
    (OUT / f"{name}.svg").write_text("\n".join(o))


M = lambda a, b, t, d=False: ("msg", a, b, t, d)
N = lambda a, t: ("note", a, t)
S = lambda t: ("sep", t)

seq("seq_telemetry",
    [("s", "Sensors\n(on the Gateway)", "dev"), ("g", "Gateway\nfirmware", "dev"), ("b", "Broker\nMosquitto", "brk"),
     ("i", "Backend\nmqtt + ingest", "be"), ("d", "Database\nSQLite", "be"), ("w", "Browser\nweb app", "web")],
    [M("s", "g", "readings every 100 ms; filtered, checked against limits"),
     N("g", "every 60 s: build the JSON message"),
     M("g", "b", "TLS handshake with certificate, then PUBLISH dt/iotmonitor/<serial>/telemetry (QoS 0)"),
     M("b", "i", "forward to the backend (it subscribed with the same certificate identity)"),
     M("i", "i", "check topic, parse JSON, pick the time"),
     M("i", "d", "INSERT telemetry row; update devices.last_seen"),
     M("i", "w", "event on the live stream (server-sent events)", True),
     M("w", "i", "GET /api/devices/<serial> (refresh the cards)"),
     M("i", "d", "SELECT latest telemetry, health, active alarms"),
     M("i", "w", "JSON: values, online flag, alarms", True),
     N("w", "the numbers on screen change, no reload")])

seq("seq_command",
    [("w", "Browser\n(admin)", "web"), ("a", "Backend\nAPI", "be"), ("d", "Database", "be"), ("b", "Broker", "brk"),
     ("g", "Gateway", "dev")],
    [M("w", "a", "click 'Read':  POST /api/devices/<id>/control  {\"command\":\"read\"}"),
     M("a", "a", "guard: valid session? role = admin? same-site request?"),
     M("a", "d", "INSERT command (status = pending)  ->  id 7"),
     M("a", "b", "PUBLISH cmd/iotmonitor/<id>/control  {\"command\":\"read\",\"req\":7}  (QoS 1)"),
     M("a", "w", "202 Accepted + the command row (pending)", True),
     M("b", "g", "deliver; the Gateway executes it"),
     M("g", "b", "PUBLISH dt/iotmonitor/<id>/control/result  {\"req\":7,\"result\":\"accepted\"}", True),
     M("b", "a", "forward -> ingest -> commands.on_result"),
     M("a", "d", "UPDATE command 7: status = accepted"),
     M("a", "w", "live event: the Commands tab shows the answer", True),
     S("no answer within 60 s:  the row is shown as 'timeout'; if the device was offline, QoS 1 delivers it later")])

seq("seq_alarm",
    [("g", "Gateway", "dev"), ("b", "Broker", "brk"), ("i", "Backend\ningest", "be"), ("n", "Notifier\nqueue + worker", "be"),
     ("p", "ntfy server", "oth"), ("ph", "Phone", "oth")],
    [N("g", "temperature crosses the limit"),
     M("g", "b", "PUBLISH dt/iotmonitor/<id>/alarms  {sensor, type HIGH, value, limit}  (QoS 1)"),
     M("b", "i", "forward; ingest stores the alarm row"),
     M("i", "n", "submit(event): which channels want 'alarm'?"),
     M("n", "n", "queue it; the MQTT thread is free again"),
     M("n", "p", "worker: HTTPS POST (up to 3 tries; max 20 messages/min/channel)"),
     M("p", "ph", "push notification", True),
     N("ph", "'Temperature HIGH on <id>'"),
     S("if the Gateway goes silent: the watcher thread sends 'device offline' once, and 'back online' once")])

seq("seq_ota",
    [("w", "Browser /\nOTA tool", "web"), ("a", "Backend\nOTA service", "be"), ("b", "Broker", "brk"),
     ("g", "Gateway\nUpdateService", "dev"), ("l", "Bootloader", "sec")],
    [M("w", "a", "PUT /api/images/a.ota and b.ota   (stored with their SHA-256)"),
     M("w", "a", "POST /api/devices/<id>/ota  {image_a, image_b}"),
     M("a", "b", "PUBLISH cmd/iotmonitor/<id>/ota  {\"job\":42,\"size_a\":N,\"size_b\":N}  (QoS 1)"),
     M("b", "g", "deliver the command"),
     M("g", "b", "result {\"result\":\"accepted\"}", True),
     S("repeat until the whole file is in the staging area (about 340 chunks of 1 KB)"),
     M("g", "b", "dt/…/ota/request  {\"job\":42,\"slot\":\"B\",\"offset\":4096,\"len\":1024}"),
     M("b", "a", "forward; backend reads that slice of the image file"),
     M("a", "b", "cmd/…/ota/data  = job | offset | 1024 bytes   (binary)", True),
     M("b", "g", "deliver; Gateway writes the chunk to QSPI staging", True),
     S("then, on the Gateway only"),
     M("g", "g", "verify: header, SHA-256 of the body, ECDSA signature with the built-in public key"),
     M("g", "g", "install into the other bank; mark TRIAL_PENDING; reset"),
     M("l", "g", "boot the new bank in trial (TRIAL_RUNNING)", True),
     M("g", "g", "self-check: sensors ready, Modbus ready, cloud connected"),
     M("g", "b", "dt/…/ota/result  {\"result\":\"success\",\"version\":\"1.0.1\"}  (after confirming)"),
     M("b", "a", "ingest.on_result -> ota job = success; an 'ota_result' alert may fire"),
     S("self-check fails or the new image crashes: the bootloader restores the old bank, result 'rolled_back'")],
    width=1000, gap=29)


# --------------------------------------------------------------------------------------------------------------------
# Back-end architecture, laid out by hand (Graphviz made a tangle of it)
# --------------------------------------------------------------------------------------------------------------------
def backend_svg():
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1130 530" font-family="%s, Arial, sans-serif">' % FONT,
         '<defs><marker id="a" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
         '<path d="M0,0 L10,5 L0,10 z" fill="#444"/></marker></defs>']

    def box(x, y, w, h, title, sub, kind, shape="rect"):
        f, st = C[kind]
        o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="9" fill="{f}" stroke="{st}" stroke-width="1.6"/>')
        o.append(f'<text x="{x + w / 2}" y="{y + (17 if sub else h / 2 + 4)}" text-anchor="middle" font-size="12.5" font-weight="700" fill="#222">{html.escape(title)}</text>')
        for i, ln in enumerate(sub.split("\n") if sub else []):
            o.append(f'<text x="{x + w / 2}" y="{y + 32 + 13 * i}" text-anchor="middle" font-size="10.5" fill="#333">{html.escape(ln)}</text>')

    def band(x, y, w, h, title, kind):
        f, st = C[kind]
        o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="12" fill="{f}55" stroke="{st}" stroke-width="1.3" stroke-dasharray="5 4"/>')
        o.append(f'<text x="{x + 12}" y="{y + 15}" font-size="11.5" font-weight="700" fill="{st}">{html.escape(title)}</text>')

    def arrow(d, text=None, tx=0, ty=0, dashed=False, both=False, anchor="middle", color="#444"):
        dash = ' stroke-dasharray="6 4"' if dashed else ""
        ms = ' marker-start="url(#a)"' if both else ""
        o.append(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="1.5"{dash} marker-end="url(#a)"{ms}/>')
        if text:
            o.append(f'<text x="{tx}" y="{ty}" text-anchor="{anchor}" font-size="10.5" fill="#222" style="paint-order:stroke" '
                     f'stroke="#fff" stroke-width="3.5">{html.escape(text)}</text>')

    # outside
    box(20, 62, 130, 58, "Mosquitto", "the broker", "brk")
    box(20, 262, 130, 56, "Browser", "the web app", "web")
    box(627, 400, 190, 42, "ntfy / webhook / e-mail", "", "oth")
    # MQTT thread
    band(175, 30, 690, 140, "MQTT thread (paho): everything the devices send", "be")
    box(195, 66, 140, 66, "mqtt.py", "client thread,\nsubscribes to dt/…", "be")
    box(370, 66, 140, 66, "ingest.py", "topic + JSON checked,\nrows written", "be")
    box(560, 52, 140, 50, "commands.py", "config / control", "be")
    box(560, 110, 140, 50, "ota.py", "images, jobs, chunks", "be")
    arrow("M150,92 H195", "", 0, 0)
    arrow("M335,99 H370")
    arrow("M510,90 L560,77")
    arrow("M510,108 L560,135")
    # event bar
    o.append('<rect x="175" y="190" width="640" height="24" rx="6" fill="#FFFBE6" stroke="#D9C46B"/>')
    o.append('<text x="495" y="206" text-anchor="middle" font-size="11" fill="#333">event: "a message of this kind just arrived from this device"</text>')
    arrow("M440,132 V190", "event", 452, 165, anchor="start")
    # API threads
    band(175, 235, 400, 145, "API threads (uvicorn): the web page and scripts", "be")
    box(195, 270, 120, 70, "api.py", "routes /api/…\nserves the web app", "be")
    box(345, 262, 210, 44, "live.py (hub)", "", "be")
    box(345, 318, 210, 44, "auth.py", "", "be")
    arrow("M150,290 L195,300", "HTTPS", 168, 281, both=True)
    arrow("M345,284 L315,296", "", dashed=False)
    arrow("M255,340 V455", "reads", 263, 420, anchor="start")
    arrow("M440,214 V262", "", 0, 0)
    arrow("M315,324 H345", "guard", 330, 334)
    arrow("M290,270 L585,160", "send command / start job", 365, 232, anchor="start")
    # alert threads
    band(595, 235, 225, 145, "Alert threads", "be")
    box(612, 262, 191, 44, "watcher", "", "be")
    box(612, 318, 191, 52, "notify.py", "queue + worker, 3 tries", "be")
    arrow("M705,214 V262", "event", 715, 244, anchor="start")
    arrow("M707,370 V400", "HTTPS / SMTP", 715, 389, anchor="start")
    # storage
    o.append('<rect x="175" y="458" width="640" height="48" rx="9" fill="%s" stroke="%s" stroke-width="1.8"/>' % C["be"])
    o.append('<text x="495" y="479" text-anchor="middle" font-size="12.5" font-weight="700" fill="#222">db.py: SQLite (one file, WAL), one connection guarded by a lock</text>')
    o.append('<text x="495" y="495" text-anchor="middle" font-size="10.5" fill="#333">telemetry, health, alarms, events, commands, ota_jobs, images, users, sessions, notify_*</text>')
    arrow("M845,170 V482 H815", "rows (ingest, commands, ota)", 855, 330, anchor="start")
    o.append('<line x1="815" y1="170" x2="845" y2="170" stroke="#444" stroke-width="1.5"/>')
    arrow("M585,300 V458", "log, last_seen", 575, 440, anchor="end")
    box(880, 52, 100, 66, "Firmware", "files on disk", "be")
    arrow("M700,135 L880,100", "read chunks", 790, 108, both=True)
    # publish back to the broker
    arrow("M630,52 V16 H85 V62", "publish: commands and firmware chunks", 360, 12, dashed=True, color="#2E8B57")
    o.append("</svg>")
    (OUT / "backend.svg").write_text("\n".join(o))


backend_svg()


# --------------------------------------------------------------------------------------------------------------------
# Hand-drawn helpers and two more diagrams (database cards, trust)
# --------------------------------------------------------------------------------------------------------------------
def canvas(w, h):
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" font-family="{FONT}, Arial, sans-serif">',
         '<defs><marker id="a" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
         '<path d="M0,0 L10,5 L0,10 z" fill="#444"/></marker></defs>']

    def txt(x, y, t, size=11, bold=False, anchor="middle", fill="#222", italic=False, halo=False):
        extra = ' style="paint-order:stroke" stroke="#fff" stroke-width="3.5"' if halo else ""
        o.append(f'<text x="{x}" y="{y}" text-anchor="{anchor}" font-size="{size}" font-weight="{700 if bold else 400}" '
                 f'font-style="{"italic" if italic else "normal"}" fill="{fill}"{extra}>{html.escape(t)}</text>')

    def box(x, y, w, h, title, sub="", kind="oth", tsize=12.5):
        f, st = C[kind]
        o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="9" fill="{f}" stroke="{st}" stroke-width="1.6"/>')
        lines = sub.split("\n") if sub else []
        ty = y + (h / 2 + 4 if not lines else 18)
        txt(x + w / 2, ty, title, tsize, True)
        for i, ln in enumerate(lines):
            txt(x + w / 2, y + 33 + 13 * i, ln, 10.5, fill="#333")

    def band(x, y, w, h, title, kind):
        f, st = C[kind]
        o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="12" fill="{f}55" stroke="{st}" stroke-width="1.3" stroke-dasharray="5 4"/>')
        txt(x + 12, y + 16, title, 11.5, True, "start", st)

    def arrow(d, label=None, lx=0, ly=0, dashed=False, color="#444", anchor="middle"):
        dash = ' stroke-dasharray="6 4"' if dashed else ""
        o.append(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="1.5"{dash} marker-end="url(#a)"/>')
        if label:
            txt(lx, ly, label, 10.5, anchor=anchor, halo=True)

    return o, txt, box, band, arrow


def card(o, txt, x, y, w, name, cols, kind="be"):
    f, st = C[kind]
    h = 24 + 15 * len(cols) + 8
    o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="7" fill="#fff" stroke="{st}" stroke-width="1.5"/>')
    o.append(f'<path d="M{x},{y + 24} V{y + 7} a7,7 0 0 1 7,-7 H{x + w - 7} a7,7 0 0 1 7,7 V{y + 24} z" fill="{st}"/>')
    txt(x + w / 2, y + 16, name, 12, True, fill="#fff")
    for i, c in enumerate(cols):
        bold = c.startswith("*")
        txt(x + 9, y + 40 + 15 * i, c.lstrip("*"), 10.2, bold, "start", "#222")
    return h


def database_svg():
    o, txt, box, band, arrow = canvas(840, 495)
    W = 196
    xs = [14, 14 + W + 12, 14 + 2 * (W + 12), 14 + 3 * (W + 12)]
    band(6, 6, 828, 150, "Data that devices send (each row says which device: the serial in column 'device')", "dev")
    card(o, txt, xs[0], 30, W, "telemetry", ["device, ts, recv_ts", "temperature_deci_c", "humidity_pct, pressure_hpa", "fd_online, fd_temperature_deci_c", "fd_humidity_pct, fd_pressure_hpa", "raw (the original JSON)"])
    card(o, txt, xs[1], 30, W, "health", ["device, ts, recv_ts", "raw (JSON with all counters:", "firmware version, uptime, resets,", "Modbus/MQTT counts, memory…)"])
    card(o, txt, xs[2], 30, W, "alarms", ["device, ts, recv_ts", "sensor_id, alarm_type", "measured_value_raw", "threshold_value_raw", "source (gateway / field_device)"])
    card(o, txt, xs[3], 30, W, "events", ["device, recv_ts", "kind: ota, config, control,", "      bad_payload", "req, result, payload (JSON)"])
    band(6, 166, 828, 128, "The cloud's own control state", "be")
    card(o, txt, xs[0], 190, W, "devices", ["*id  (the device serial)", "first_seen, last_seen", "fw_version", "last_health (latest JSON)"])
    card(o, txt, xs[1], 190, W, "commands", ["*id (= the 'req' number)", "device, kind, body", "created, finished", "status, reason"])
    card(o, txt, xs[2], 190, W, "ota_jobs", ["*id (= the job number)", "device, image_a, image_b", "state, sent_bytes", "detail (last report)"])
    card(o, txt, xs[3], 190, W, "images", ["*name", "size, sha256, uploaded", "(the files are on disk)"])
    band(6, 304, 828, 150, "Login and alerts", "web")
    card(o, txt, xs[0], 328, W, "users", ["*name, role", "pw_hash (scrypt)"])
    card(o, txt, xs[1], 328, W, "sessions", ["*token_hash (SHA-256)", "user, created, expires"])
    card(o, txt, xs[2], 328, W, "notify_channels", ["*id, name, kind", "config (secrets inside)", "events wanted, enabled"])
    card(o, txt, xs[3], 328, W, "notify_log", ["ts, channel, event, device", "title, status, error"])
    card(o, txt, xs[0], 403, W, "device_state", ["*device, offline_since"], "be")
    txt(420, 480, "device_state remembers which devices were already reported offline, so a restart does not repeat the alert.", 10.5, anchor="middle", fill="#555", italic=True)
    o.append("</svg>")
    (OUT / "database.svg").write_text("\n".join(o))


def trust_svg():
    o, txt, box, band, arrow = canvas(860, 470)
    band(6, 6, 848, 262, "1. Who is this program, and what may it do?  (certificates + access list)", "sec")
    box(24, 110, 190, 66, "Our Certificate Authority", "~/.iot-pki/ca\nkey is encrypted", "sec")
    box(300, 36, 210, 46, "Broker certificate", "", "brk")
    box(300, 96, 210, 46, "Backend certificate: iotcloud", "", "be")
    box(300, 156, 210, 46, "Operator certificate: ota-operator", "", "sec")
    box(300, 216, 210, 46, "Device certificate (its serial)", "", "dev")
    for y in (59, 119, 179, 239):
        arrow(f"M214,143 L300,{y}", "signs" if y == 59 else None, 250, 85, anchor="middle")
    box(610, 96, 230, 110, "Access list (acl.conf)", "each NAME may publish and\nread only certain topics.\nName = the certificate's\ncommon name", "brk")
    arrow("M510,119 H610", "")
    arrow("M510,179 H560 L610,165")
    arrow("M510,239 H570 L610,190")
    txt(560, 76, "'I am the broker':", 10.5, fill="#2E8B57", italic=True)
    txt(560, 89, "devices check this one", 10.5, fill="#2E8B57", italic=True)
    band(6, 280, 420, 180, "2. Is this firmware genuine?", "dev")
    box(24, 312, 170, 56, "Release signing key", "private, on the PC", "sec")
    box(240, 312, 170, 56, "Public half", "built into the firmware", "dev")
    arrow("M194,340 H240", "matching pair", 217, 331)
    txt(216, 400, "Every update's header is signed with the private key.", 10.5, fill="#333")
    txt(216, 415, "The Gateway checks it with the public half", 10.5, fill="#333")
    txt(216, 430, "before installing anything. A different key from the CA.", 10.5, fill="#333")
    band(438, 280, 416, 180, "3. Who is the person at the web page?", "web")
    box(456, 312, 170, 56, "Caddy's own small CA", "certificate for envmon.lan", "web")
    box(672, 312, 166, 56, "Browser", "imported Caddy's root once", "web")
    arrow("M626,340 H672", "trusted", 649, 331)
    box(456, 392, 382, 56, "Login", "user + password (scrypt) → session cookie, roles admin / viewer", "web")
    o.append("</svg>")
    (OUT / "trust.svg").write_text("\n".join(o))


database_svg()
trust_svg()

print("diagrams written to", OUT, file=sys.stderr)
