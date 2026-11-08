#!/usr/bin/env python3
"""Write the Doxygen main page of each board to generated-docs/doxygen/mainpage-<board>.md.

Generated from the folders (module list, integration tests) and from the #include lines (the dependency graph),
so it does not go stale; the explanations and the command lists are the texts below. Every module name links to
its public headers (a bad link shows up as a Doxygen warning). Needs graphviz (dot). Called by scripts/gen-doxygen.sh.
"""
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "generated-docs/doxygen"
SKIP_HEADERS = {"lv_conf.h", "mbedtls_config_gateway.h"}  # not in the Doxygen input (third-party configuration)

BOARDS = {
    "gateway": (
        "Gateway firmware",
        "STM32L475 (B-L475E-IOT01A). Reads its own sensors, polls the Field Device over Modbus RTU (RS-485), "
        "and publishes telemetry, health and alarms to the cloud over MQTT with mutual TLS. Receives OTA updates "
        "into two firmware slots with rollback.",
    ),
    "field-device": (
        "Field Device firmware",
        "STM32F469 (32F469IDISCOVERY). Reads temperature, humidity and pressure, evaluates the alarms, shows them "
        "on the touch display, and serves them to the Gateway as a Modbus RTU slave.",
    ),
}
# used by almost every module: left out of the graph so the structure stays readable
CROSS_CUTTING = {"logger"}

LAYERS = {
    "application": ("Application", "the services that implement the product behaviour", "#cfe8ff"),
    "middleware": ("Middleware", "reusable components built on the drivers", "#d9f2d9"),
    "drivers": ("Drivers", "access to the hardware", "#ffe9c7"),
}

REPO_MAP = [
    ("firmware/gateway, firmware/field-device", "The two firmwares. Each has `application/`, `middleware/`, `drivers/` (the layers), `integration-tests/` (bring-up tests for the board), `Src/` (the product `main.c`) and the linker scripts."),
    ("firmware/shared", "Headers used by both boards (Modbus frame, firmware version)."),
    ("vendor", "Third-party code, used as delivered: FreeRTOS, coreMQTT, mbedTLS, LVGL, STM32 HAL."),
    ("tests", "Host unit tests (Ceedling, Unity, CMock). `tests/gateway` and `tests/field-device` mirror the firmware folders."),
    ("verification", "System tests (Robot Framework) that drive the real boards and the cloud broker."),
    ("simulator/lcd-ui", "Runs the Field Device display on the PC (LVGL with SDL2) and renders every screen."),
    ("scripts", "Build, flash, bench and check scripts."),
    ("docs", "Requirements (`SRS.md`), design (`hld/`, `lld/`), test plan and results (`test/`), this Doxygen set (`doxygen/`)."),
]

# (title, summary, [(comment, command)])
TOOLS = [
    ("Build the firmware images",
     "Builds an image without the IDE, with the flags of the CubeIDE Debug configuration, and fails if it does not fit the chip. "
     "Output goes to `firmware/<board>/Debug/app/` (not committed). Needs `arm-none-eabi-gcc`; run the version generator once first.",
     [("version header, once", "python3 scripts/gen-version.py firmware/shared/firmware_version.h.in firmware/shared/firmware_version.h"),
      ("Field Device with the display", "GUI=1 bash scripts/build-fd-app.sh"),
      ("Field Device, build and program the board", "GUI=1 ST_SERIAL=<st-link serial> bash scripts/build-fd-app.sh --flash"),
      ("Gateway (needs firmware/gateway/certs/, see scripts/bringup_secrets.h.example)", "bash scripts/build-gateway-app.sh"),
      ("Gateway, build and program the board", "ST_SERIAL=<st-link serial> bash scripts/build-gateway-app.sh --flash"),
      ("Gateway image for a firmware slot (A or B)", "SLOT=A bash scripts/build-gateway-app.sh"),
      ("signed release images", "bash scripts/build-release.sh --version X.Y.Z --private KEY.pem --out DIR")]),
    ("Host unit tests",
     "Each module is compiled against mocks of its neighbours and run on the PC, with no hardware. Ceedling finds every `tests/**/test_*.c`. "
     "The Gateway modules whose names collide with a Field Device module have their own project file. CI runs both on every push.",
     [("Field Device and shared modules", "cd tests && ceedling test:all"),
      ("Gateway modules", "cd tests && CEEDLING_MAIN_PROJECT_FILE=project_gateway.yml ceedling test:all"),
      ("one test file", "cd tests && ceedling test:test_modbus_register_map"),
      ("one module: tests, cppcheck and clang-format together", "./scripts/test-module.sh -m gw-<module>    # or fd-<module>; add -f to apply the formatting")]),
    ("Static analysis and formatting",
     "cppcheck and clang-format 17 run on every module, the integration tests, the bootloader and the shared headers; CI fails on any finding. "
     "Two more checks keep the build configurations honest.",
     [("static analysis of a folder", "cppcheck --enable=style,warning,performance -I firmware/shared --suppressions-list=cppcheck-suppressions.txt --inline-suppr --error-exitcode=1 firmware/gateway/application/<module>"),
      ("formatting, check", "clang-format-17 --dry-run --Werror <files>"),
      ("formatting, apply", "clang-format-17 -i <files>"),
      ("each CubeIDE configuration compiles exactly one main()", "bash scripts/check-build-exclusions.sh"),
      ("HAL and BSP headers only in the LCD driver", "bash scripts/check-hal-encapsulation.sh")]),
    ("Integration tests on the board",
     "One bring-up image per module exercises it on the real hardware and prints PASS or FAIL lines on the serial console "
     "(the header of each test says the wiring and the serial settings). On the Field Device the test replaces `Src/main.c` through `MAIN`; "
     "on the Gateway the header of the test describes the project swap, which is bench-local and not committed.",
     [("Field Device module test, build and program", "MAIN=firmware/field-device/integration-tests/<module>/test_<module>_main.c bash scripts/build-fd-app.sh --flash"),
      ("watch the console (Field Device 115 200, Gateway 460 800)", "python3 -m serial.tools.miniterm /dev/ttyACM0 115200"),
      ("scripted benches", "python3 scripts/console_bench.py ...   # also alarm_latency_bench.py, two_board_monitor.py, bootloader_bench.py")]),
    ("Display simulator",
     "Runs the Field Device user interface on the PC and renders every screen and state to PNG files, to review the display without the board.",
     [("build and render (as CI does)", "cd simulator/lcd-ui && cmake -S . -B build-native -G Ninja && cmake --build build-native && bash shots.sh build-native/lcd_sim shots")]),
    ("System verification",
     "Robot Framework suites, in waves, drive both boards through their consoles and read the cloud messages from a Mosquitto broker. "
     "Wave 0 needs no hardware; waves 1 to 6 need the boards; wave 6 is the 8 hour endurance run; wave 7 is the operator checklist. "
     "The merger turns the runs into the requirement status.",
     [("python environment, once", "python3 -m venv ~/.venvs/envmon-verify && ~/.venvs/envmon-verify/bin/pip install robotframework pyserial pillow paho-mqtt"),
      ("a wave, or several", "verification/run.sh 0        # or: verification/run.sh 1 2 3"),
      ("one test", "verification/run.sh --test 'TC-VER-GW-011*' 3"),
      ("merge the results into the report", "python3 tools/merge-verification-results.py        # --check fails if a test failed")]),
    ("This documentation",
     "Doxygen with Graphviz. The default run documents the public headers and must end with 0 warnings (the REQ-NF-505 check).",
     [("both boards", "scripts/gen-doxygen.sh"),
      ("with the .c files, for call graphs", "scripts/gen-doxygen.sh --with-sources"),
      ("one board", "scripts/gen-doxygen.sh gateway")]),
]


def mangle(name):
    return name.replace("_", "__").replace(".", "_8").replace("-", "_") + ".html"


def headers_of(mod):
    return sorted(h for h in mod.rglob("*.h") if h.name not in SKIP_HEADERS and "integration-tests" not in h.parts)


def modules(board):
    """{(layer, name): (path, [headers])}"""
    res = {}
    for layer in LAYERS:
        d = ROOT / "firmware" / board / layer
        if d.is_dir():
            for m in sorted(p for p in d.iterdir() if p.is_dir()):
                res[(layer, m.name)] = (m, headers_of(m))
    return res


def main_header(name, headers):
    for h in headers:
        if h.stem == name:
            return h
    return headers[0] if headers else None


def dependency_graph(board, mods):
    owner = {}
    for key, (path, hs) in mods.items():
        for h in hs:
            owner.setdefault(h.name, key)
    edges = set()
    inc = re.compile(r'#\s*include\s+"([^"]+)"')
    for key, (path, _) in mods.items():
        for f in list(path.rglob("*.c")) + list(path.rglob("*.h")):
            if "integration-tests" in f.parts:
                continue
            for m in inc.finditer(f.read_text(errors="ignore")):
                tgt = owner.get(Path(m.group(1)).name)
                if tgt and tgt != key and tgt[1] not in CROSS_CUTTING:
                    edges.add((key, tgt))
    dot = ['digraph G {', 'rankdir=TB; nodesep=0.3; ranksep=1.1;',
           'node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=11, height=0.3];',
           'edge [color="#777777", arrowsize=0.6];']
    for layer, (title, desc, colour) in LAYERS.items():
        names = [k for k in mods if k[0] == layer]
        if not names:
            continue
        dot.append(f'subgraph cluster_{layer} {{ label="{title}"; style="rounded"; color="#999999"; fontname="Helvetica-Bold"; fontsize=13;')
        for k in names:
            mh = main_header(k[1], mods[k][1])
            url = f', URL="{mangle(mh.name)}", tooltip="{k[1]}"' if mh else ""
            dot.append(f'"{k[0]}/{k[1]}" [label="{k[1]}", fillcolor="{colour}"{url}];')
        dot.append("}")
    order = {k: i for i, k in enumerate(LAYERS)}
    for a, b in sorted(edges):
        d = order[b[0]] - order[a[0]]
        style = ('class="e-down", color="#9aa5b1"' if d > 0 else 'class="e-same", color="#2b7bd6"' if d == 0
                 else 'class="e-up", color="#d6342b", penwidth=1.8')
        dot.append(f'"{a[0]}/{a[1]}" -> "{b[0]}/{b[1]}" [{style}];')
    dot.append("}")
    return "\n".join(dot), len(edges)


def repo_graph():
    dot = ['digraph R { rankdir=LR; nodesep=0.3; node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=11];',
           '"fw" [label="firmware/gateway\\nfirmware/field-device\\n(application, middleware, drivers)", fillcolor="#cfe8ff"];',
           '"shared" [label="firmware/shared", fillcolor="#cfe8ff"];',
           '"vendor" [label="vendor\\n(FreeRTOS, coreMQTT, mbedTLS, LVGL, HAL)", fillcolor="#eeeeee"];',
           '"tests" [label="tests\\n(Ceedling host unit tests)", fillcolor="#d9f2d9"];',
           '"it" [label="firmware/*/integration-tests\\n(bring-up on the board)", fillcolor="#d9f2d9"];',
           '"ver" [label="verification\\n(Robot Framework, both boards)", fillcolor="#d9f2d9"];',
           '"sim" [label="simulator/lcd-ui\\n(display on the PC)", fillcolor="#d9f2d9"];',
           '"scripts" [label="scripts\\n(build, flash, bench, checks)", fillcolor="#ffe9c7"];',
           '"docs" [label="docs\\n(SRS, hld, lld, test, doxygen)", fillcolor="#fff3b0"];',
           '"fw" -> "shared" [label="uses"]; "fw" -> "vendor" [label="uses"];',
           '"tests" -> "fw" [label="mocks the neighbours"]; "it" -> "fw" [label="exercises on target"];',
           '"ver" -> "fw" [label="drives the boards"]; "sim" -> "fw" [label="compiles lcd_ui"];',
           '"scripts" -> "fw" [label="builds, flashes"]; "docs" -> "fw" [label="specifies", style=dashed]; }']
    return "\n".join(dot)


def svg(dot_text):
    out = subprocess.run(["dot", "-Tsvg"], input=dot_text, capture_output=True, text=True, check=True).stdout
    out = out[out.index("<svg"):]
    out = re.sub(r'<svg width="[^"]+" height="[^"]+"', '<svg style="max-width:100%;height:auto"', out, count=1)
    return out


GRAPH_JS = """<script>
(function () {
  var box = document.getElementById("%(id)s");
  if (!box) return;
  var edges = box.querySelectorAll("g.edge"), nodes = box.querySelectorAll("g.node");
  function title(g) { var t = g.querySelector("title"); return t ? t.textContent : ""; }
  function focus(name) {
    edges.forEach(function (e) {
      var t = title(e), on = name && (t.indexOf(name + "->") === 0 || t.slice(-name.length - 2) === "->" + name);
      e.style.opacity = !name ? "" : (on ? "1" : "0.06");
      e.style.strokeWidth = on ? "2.5" : "";
    });
    nodes.forEach(function (n) { n.style.opacity = !name || title(n) === name || connected(name, title(n)) ? "" : "0.35"; });
  }
  function connected(a, b) {
    for (var i = 0; i < edges.length; i++) {
      var t = title(edges[i]);
      if (t === a + "->" + b || t === b + "->" + a) return true;
    }
    return false;
  }
  nodes.forEach(function (n) {
    n.addEventListener("mouseenter", function () { focus(title(n)); });
    n.addEventListener("mouseleave", function () { focus(""); });
  });
  var q = /[?&]focus=([^&]+)/.exec(location.search);
  if (q) focus(decodeURIComponent(q[1]));
})();
</script>"""

LEGEND = ('<p style="margin:0.3em 0"><b>Arrows:</b> '
          '<span style="color:#9aa5b1">&#9644; uses a lower layer</span> &nbsp; '
          '<span style="color:#2b7bd6">&#9644; uses a module of the same layer</span> &nbsp; '
          '<span style="color:#d6342b">&#9644; uses a higher layer (look at these)</span>. '
          '<b>Hover</b> a module to show only its arrows; <b>click</b> it to open its header.</p>')


def md_modules(board, mods):
    L = []
    for layer, (title, desc, _) in LAYERS.items():
        names = [k for k in mods if k[0] == layer]
        if not names:
            continue
        L += [f"### {title}", f"The {desc}.", ""]
        for k in names:
            hs = mods[k][1]
            main = [h for h in hs if h.stem == k[1]]
            links = ", ".join(f"\\ref {h.name}" for h in main + [h for h in hs if h not in main]) or "(no public header)"
            L.append(f"- **{k[1]}**: {links}")
        L.append("")
    return L


def md_tools():
    L = []
    for title, summary, cmds in TOOLS:
        L += [f"### {title}", "", summary, "", "```sh"]
        for comment, cmd in cmds:
            L += [f"# {comment}", cmd]
        L += ["```", ""]
    return L


def page(board):
    title, blurb = BOARDS[board]
    mods = modules(board)
    dot, n_edges = dependency_graph(board, mods)
    L = [f"# IoT Environmental Monitor: {title}", "", blurb, "",
         "This page is the starting point: how the code is organised, how to find things in this reference, and the command that runs each tool "
         "and test. The reference itself covers the **public headers** (one page per header and per struct).", "",
         "## The repository", "",
         "```", "firmware/<board>/", "    application/   services: the product behaviour",
         "    middleware/    reusable components on top of the drivers",
         "    drivers/       hardware access",
         "    integration-tests/   bring-up test per module, run on the board",
         "    Src/           product main()", "```", ""]
    L += ["| Folder | What is in it |", "|---|---|"] + [f"| `{a}` | {b} |" for a, b in REPO_MAP] + [""]
    L += ["\\htmlonly", '<div style="overflow:auto;margin:1em 0">', svg(repo_graph()), "</div>", "\\endhtmlonly", ""]
    L += ["## How the code is layered", "",
          "Dependencies mostly point **downwards**: application services use middleware and drivers, middleware uses drivers, drivers touch the hardware. "
          "The arrows that go up are the exceptions to look at: lower layers report their faults to `health_monitor`, an application service. "
          "Modules reach each other through interface tables (the `i...` structs of Data Structures), which is also how the tests replace a neighbour with a mock. "
          f"The graph below is generated from the `#include` lines of this board ({len(mods)} modules, {n_edges} dependencies; the `logger`, used by almost every module, is left out of the arrows); "
          "click a module to open its header.", "",
          "\\htmlonly", LEGEND, f'<div id="graph-{board}" style="overflow:auto;margin:1em 0;border:1px solid #ccc;padding:0.5em">', svg(dot), "</div>",
          GRAPH_JS % {"id": f"graph-{board}"}, "\\endhtmlonly", "",
          "## How to navigate this reference", "",
          "1. Find the module in the graph above or in the list below and open its main header: the first comment of the file says what the module is for.",
          "2. Open the **interface** (`i...` header, or the vtable struct in the main header) to see what the rest of the system can call.",
          "3. **Data Structures** (menu): every struct, with a UML-style diagram of its members and references. Use it to follow a type from one module to the next.",
          "4. **Files**: the headers by folder, with the include graph of each. **Search** (top right) finds any function, type or macro.",
          "5. For the behaviour behind a module, read its low-level design in `docs/lld/` and its test: the unit test in `tests/`, and the bring-up test in `firmware/" + board + "/integration-tests/`.", "",
          "## Modules", ""]
    L += md_modules(board, mods)
    L += ["## Tools and how to run them", "",
          "All commands run from the repository root unless a `cd` says otherwise. CI (`.github/workflows/ci.yml`) runs the host tests, "
          "the firmware builds, the display simulator, cppcheck and clang-format on every push.", ""]
    L += md_tools()
    L += ["## Tests, from cheap to complete", "",
          "| Level | Where | Runs on | Answers |", "|---|---|---|---|",
          "| Unit | `tests/` | PC | Does the module do what its design says, with its neighbours mocked? |",
          f"| Integration | `firmware/{board}/integration-tests/` | the board | Does the module work on the real hardware? |",
          "| System | `verification/` | both boards, broker | Do the two boards and the cloud meet the requirements together? Includes the 8 hour endurance run. |", "",
          "### Integration tests of this board", ""]
    d = ROOT / "firmware" / board / "integration-tests"
    names = sorted(p.name for p in d.iterdir() if p.is_dir()) if d.is_dir() else []
    L += [", ".join(f"`{n}`" for n in names) if names else "(none)", "",
          "## Related documents", "",
          "- Requirements and design: `docs/SRS.md`, `docs/hld/`, `docs/lld/`.",
          "- Register map: `docs/hld/modbus-register-map.md`.",
          "- Test plan, results and defects: `docs/test/`.", ""]
    return "\n".join(L)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for b in BOARDS:
        (OUT / f"mainpage-{b}.md").write_text(page(b))


if __name__ == "__main__":
    main()
