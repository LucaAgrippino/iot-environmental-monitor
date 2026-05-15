# usage: python scripts/gate_review_check.py
"""
Gate Review Mechanical Consistency Checker
Phase 2 → Phase 3 gate check for IoT Environmental Monitor HLD.
"""

import re
import sys
from datetime import date
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DOCS = REPO_ROOT / "docs"

# ── Input file catalogue ──────────────────────────────────────────────────────

REQUIRED_INPUTS = {
    "vision":           DOCS / "vision.md",
    "srs":              DOCS / "SRS.md",
    "use_cases":        DOCS / "use-case-descriptions.md",
    "hld":              DOCS / "hld" / "hld.md",
    "components":       DOCS / "hld" / "components.md",
    "state_machines":   DOCS / "hld" / "state-machines.md",
    "sequences":        DOCS / "hld" / "sequence-diagrams.md",
    "task_breakdown":   DOCS / "hld" / "task-breakdown.md",
    "modbus":           DOCS / "hld" / "modbus-register-map.md",
    "flash_layout":     DOCS / "hld" / "flash-partition-layout.md",
    "colour_palette":   DOCS / "diagram-colour-palette.md",
    "arch_principles":  DOCS / "architecture-principles.md",   # optional
}

HLD_DOCS_KEYS = [
    "hld", "components", "state_machines", "sequences",
    "task_breakdown", "modbus", "flash_layout",
]

# PNG directories to check (both canonical and alternate)
PNG_DIRS = [
    DOCS / "hld" / "diagrams",
    DOCS / "diagrams",
]

# ── Helpers ───────────────────────────────────────────────────────────────────

def read(path: Path) -> str | None:
    """Return file contents or None if missing."""
    if not path.exists():
        return None
    return path.read_text(encoding="utf-8", errors="replace")


OUT_REPORT = DOCS / "hld" / "gate-review" / "mechanical-report.md"


def all_md_files() -> list[Path]:
    return [p for p in sorted(DOCS.rglob("*.md")) if p.resolve() != OUT_REPORT.resolve()]


def hld_texts(inputs: dict) -> dict[str, str]:
    """Return {key: text} for HLD documents that exist."""
    result = {}
    for k in HLD_DOCS_KEYS:
        txt = inputs.get(k)
        if txt is not None:
            result[k] = txt
    return result


REQ_PAT  = re.compile(r'\bREQ-[A-Z]{2,3}-\d{3}\b')
DECISION_PAT = re.compile(r'\b(D\d+)\b(?=\s*[–—-])')
DECISION_BARE = re.compile(r'\b(D\d+)\b')
UC_PAT   = re.compile(r'\bUC-\d{2}\b')
IFACE_PAT = re.compile(r'\bI[A-Z][A-Za-z]+\b')
PASCAL_PAT = re.compile(r'\b[A-Z][a-z][A-Za-z0-9]*(?:[A-Z][a-zA-Z0-9]*)+\b')
LINK_PAT  = re.compile(r'\[(?:[^\]]*)\]\(([^)#][^)]*)\)')
OPEN_PAT  = re.compile(r'\b(TBD|TODO|FIXME|XXX|\?\?\?)\b')

# ── Structural extraction helpers ─────────────────────────────────────────────

def _structural_components(comp_text: str) -> set[str]:
    """
    Extract component names from structural positions in components.md:
      **NAME:** lines, bold **ComponentName** tokens, table cells whose
      entire trimmed content is a single PascalCase identifier, and bullet
      list items of the form '- ComponentName'.
    Token filter: starts with uppercase, contains at least one lowercase,
    length >= 3 (rejects all-caps acronyms like CMSIS, GPIO).
    """
    names: set[str] = set()
    COMP_TOK = re.compile(r'^[A-Z][A-Za-z0-9]*[a-z][A-Za-z0-9]*$')

    for raw_line in comp_text.splitlines():
        line = raw_line.strip()

        # **NAME:** ComponentName  (spec blocks — most comprehensive source)
        m = re.match(r'\*\*NAME:\*\*\s+(\S+)', line)
        if m:
            tok = m.group(1)
            if COMP_TOK.match(tok):
                names.add(tok)

        # **ComponentName** bold prose (label lines have colons inside the bold,
        # so **NAME:** / **LAYER:** etc. are never captured here)
        for m in re.finditer(r'\*\*([A-Z][A-Za-z0-9]+)\*\*', line):
            tok = m.group(1)
            if COMP_TOK.match(tok):
                names.add(tok)

        # Table cells: entire trimmed cell is a single PascalCase identifier
        if line.startswith('|') and not re.match(r'^\|[-:\s|]+$', line):
            for cell in line.split('|'):
                cell = cell.strip()
                if COMP_TOK.match(cell):
                    names.add(cell)

        # Bullet list items: - ComponentName (optional trailing annotation)
        m = re.match(r'^[-*]\s+([A-Z][A-Za-z0-9]+)', line)
        if m:
            tok = m.group(1)
            if COMP_TOK.match(tok):
                names.add(tok)

    # Exclude structural/prose keywords that pass the token filter
    _COMP_EXCLUDE = {
        'Application', 'Middleware', 'Driver', 'Hardware', 'Software',
        'Component', 'Interface', 'Peripheral', 'Layer',
    }
    names -= _COMP_EXCLUDE
    return names


def _structural_interfaces(comp_text: str) -> set[str]:
    """
    Extract interface names from **PROVIDES (upward):** lines (authoritative
    definition point) and bold **IXxx** prose tokens in components.md.
    Uses a slightly broader token pattern than IFACE_PAT so that names like
    II2c (I2cDriver's interface) are captured.
    """
    names: set[str] = set()
    IFACE_TOK = re.compile(r'\bI[A-Za-z][A-Za-z0-9]+\b')

    for raw_line in comp_text.splitlines():
        line = raw_line.strip()
        if re.match(r'\*\*PROVIDES \(upward\):\*\*', line):
            for m in IFACE_TOK.finditer(line):
                names.add(m.group(0))
        for m in re.finditer(r'\*\*(I[A-Za-z][A-Za-z0-9]+)\*\*', line):
            names.add(m.group(1))

    return {n for n in names if len(n) >= 4}

# ── Check 1: Requirement-ID cross-reference ───────────────────────────────────

def check_requirements(inputs: dict) -> tuple[list[str], bool]:
    defects = []
    inconclusive = False

    srs_text = inputs.get("srs")
    if srs_text is None:
        return ["Inconclusive: SRS.md is missing"], True

    srs_ids = set(REQ_PAT.findall(srs_text))

    hld_all_text = "\n".join(
        t for k, t in inputs.items()
        if k not in ("srs", "vision", "use_cases", "colour_palette", "arch_principles")
        and t is not None
    )
    hld_ids = set(REQ_PAT.findall(hld_all_text))

    broken = sorted(hld_ids - srs_ids)
    orphan = sorted(srs_ids - hld_ids)

    for r in broken:
        defects.append(f"Broken trace: {r} cited in HLD but not defined in SRS")
    for r in orphan:
        defects.append(f"Orphan: {r} defined in SRS but never cited in any HLD document")

    # Check declared count vs extracted count in SRS §6 Traceability Matrix
    # Look for a line like "| Total | N |" or "N requirements" in a traceability section
    tmat = re.search(
        r'(?:traceability|§\s*6)[^\n]*\n(.*?)(?:\n##|\Z)',
        srs_text, re.IGNORECASE | re.DOTALL
    )
    declared_count = None
    if tmat:
        nums = re.findall(r'\|\s*Total\s*\|\s*(\d+)', tmat.group(1), re.IGNORECASE)
        if nums:
            declared_count = int(nums[-1])

    # Also try pattern: "N requirements" near traceability heading
    if declared_count is None:
        m = re.search(r'(\d+)\s+requirements?\s+are\s+traced', srs_text, re.IGNORECASE)
        if m:
            declared_count = int(m.group(1))

    # Count REQs defined in §2–§3 of SRS (exclude §6 itself)
    # Heuristic: count unique REQ IDs in the SRS body
    body_ids = set(REQ_PAT.findall(srs_text))
    if declared_count is not None and declared_count != len(body_ids):
        defects.append(
            f"Count mismatch: §6 declares {declared_count} requirements, "
            f"extracted {len(body_ids)} unique IDs from SRS"
        )

    return defects, inconclusive


# ── Check 2: Decision-ID coherence ───────────────────────────────────────────

def check_decisions(inputs: dict) -> tuple[list[str], bool]:
    defects = []
    inconclusive = False

    hld_text = inputs.get("hld")
    if hld_text is None:
        return ["Inconclusive: hld.md is missing"], True

    # IDs defined in master decision log (lines with D\d+ followed by dash/em-dash)
    master_ids: set[int] = set()
    for line in hld_text.splitlines():
        for m in DECISION_PAT.finditer(line):
            master_ids.add(int(m.group(1)[1:]))

    # Fallback: collect all D\d+ from hld.md
    if not master_ids:
        for m in DECISION_BARE.finditer(hld_text):
            master_ids.add(int(m.group(1)[1:]))

    # IDs in companion documents
    companion_ids: set[int] = set()
    companions = {k: v for k, v in inputs.items()
                  if k not in ("srs", "vision", "use_cases", "colour_palette",
                               "arch_principles", "hld")
                  and v is not None}
    for txt in companions.values():
        for m in DECISION_BARE.finditer(txt):
            num = int(m.group(1)[1:])
            companion_ids.add(num)

    all_ids = master_ids | companion_ids
    if all_ids:
        sorted_ids = sorted(all_ids)
        for i in range(sorted_ids[0], sorted_ids[-1] + 1):
            if i not in all_ids:
                defects.append(f"Gap in Decision IDs: D{i} is missing")

    master_only = master_ids - companion_ids
    companion_only = companion_ids - master_ids

    for i in sorted(master_only):
        defects.append(f"D{i} in master hld.md decision log but not in any companion document")
    for i in sorted(companion_only):
        defects.append(f"D{i} in companion document(s) but not in master hld.md decision log")

    return defects, inconclusive


# ── Check 3: Component-name consistency ──────────────────────────────────────

def check_components(inputs: dict) -> tuple[list[str], bool]:
    defects = []
    inconclusive = False

    comp_text = inputs.get("components")
    if comp_text is None:
        return ["Inconclusive: components.md is missing"], True

    hld_text = inputs.get("hld")

    component_names = _structural_components(comp_text)

    # Gather all companion texts
    companion_texts: dict[str, str] = {}
    for k, v in inputs.items():
        if k not in ("srs", "vision", "use_cases", "colour_palette", "arch_principles") and v is not None:
            companion_texts[k] = v

    full_hld_text = "\n".join(companion_texts.values())

    for name in sorted(component_names):
        if hld_text and name not in hld_text:
            defects.append(f"Component '{name}' not referenced in hld.md")
        found_in_companion = any(
            name in txt for k, txt in companion_texts.items() if k != "hld"
        )
        if not found_in_companion:
            defects.append(f"Component '{name}' not referenced in any companion document")

    # Reverse: PascalCase appearing frequently in companions but absent from components.md
    all_companion_text = "\n".join(
        v for k, v in companion_texts.items() if k not in ("hld", "components")
    )
    candidate_counts: dict[str, int] = {}
    for m in PASCAL_PAT.finditer(all_companion_text):
        name = m.group(0)
        if len(name) >= 4:
            candidate_counts[name] = candidate_counts.get(name, 0) + 1

    THRESHOLD = 3
    for name, count in sorted(candidate_counts.items()):
        if count >= THRESHOLD and name not in component_names:
            # Exclude common non-component words
            skip = {"True", "False", "None", "This", "With", "That", "When",
                    "Each", "Both", "From", "Upon", "After", "Before", "Since",
                    "While", "Using", "Note", "Return", "Error", "Data", "File",
                    "Task", "Event", "State", "Type", "Time", "Line", "Item",
                    "List", "Table", "Field", "Value", "Modbus", "Gateway",
                    "Device", "System", "Cloud", "Sensor", "Alarm", "Flash",
                    "Queue", "Timer", "Buffer", "Config", "Status", "Health",
                    "Telemetry", "Firmware", "Update", "Register", "Address"}
            if name not in skip:
                defects.append(
                    f"'{name}' appears {count}× in companion docs but not in components.md"
                )

    return defects, inconclusive


# ── Check 4: Interface-name consistency ──────────────────────────────────────

def check_interfaces(inputs: dict) -> tuple[list[str], bool]:
    defects = []
    inconclusive = False

    comp_text = inputs.get("components")
    if comp_text is None:
        return ["Inconclusive: components.md is missing"], True

    iface_names = _structural_interfaces(comp_text)

    hld_texts_map = {
        k: v for k, v in inputs.items()
        if k not in ("srs", "vision", "use_cases", "colour_palette", "arch_principles")
        and v is not None
    }
    full_text = "\n".join(hld_texts_map.values())

    hld_text = inputs.get("hld", "")

    for name in sorted(iface_names):
        if hld_text and name not in hld_text:
            defects.append(f"Interface '{name}' defined in components.md but not referenced in hld.md")
        companion_text = "\n".join(
            v for k, v in hld_texts_map.items() if k not in ("hld", "components")
        )
        if name not in companion_text:
            defects.append(f"Interface '{name}' defined in components.md but not referenced in any companion document")

    # Reverse: interfaces in companion docs not in components.md
    all_text = "\n".join(
        v for k, v in hld_texts_map.items() if k != "components" and v is not None
    )
    found_ifaces = set(IFACE_PAT.findall(all_text))
    found_ifaces = {n for n in found_ifaces if len(n) >= 4}
    for name in sorted(found_ifaces - iface_names):
        defects.append(
            f"Interface '{name}' referenced in HLD docs but not defined in components.md"
        )

    return defects, inconclusive


# ── Check 5: Diagram embedding ────────────────────────────────────────────────

def check_diagrams() -> tuple[list[str], bool]:
    defects = []
    inconclusive = False

    all_pngs: list[Path] = []
    for d in PNG_DIRS:
        if d.exists():
            all_pngs.extend(d.glob("*.png"))

    if not all_pngs:
        return ["Inconclusive: no PNG diagrams found in docs/hld/diagrams/ or docs/diagrams/"], True

    # Build set of all PNG filenames referenced across all markdown
    all_md_text = ""
    md_ref_names: set[str] = set()
    for md in all_md_files():
        text = md.read_text(encoding="utf-8", errors="replace")
        all_md_text += text
        # Match both ![alt](path/to/file.png) and bare path mentions
        for m in re.finditer(r'([A-Za-z0-9_.\-/]+\.png)', text):
            md_ref_names.add(Path(m.group(1)).name)

    # Orphan PNGs: exist but never referenced
    for png in sorted(all_pngs):
        if png.name not in md_ref_names:
            defects.append(f"Orphan PNG: {png.relative_to(REPO_ROOT)} is never referenced in any .md file")

    # Markdown references to non-existent PNGs
    existing_png_names = {p.name for p in all_pngs}
    for md in all_md_files():
        text = md.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r'([A-Za-z0-9_.\-/]+\.png)', text):
            ref_name = Path(m.group(1)).name
            if ref_name not in existing_png_names:
                defects.append(
                    f"Broken PNG ref: '{m.group(1)}' in {md.relative_to(REPO_ROOT)} — file does not exist"
                )

    return defects, inconclusive


# ── Check 6: Markdown link integrity ─────────────────────────────────────────

def check_links() -> tuple[list[str], bool]:
    defects = []

    for md in all_md_files():
        text = md.read_text(encoding="utf-8", errors="replace")
        for m in LINK_PAT.finditer(text):
            target = m.group(1).strip()
            # Skip URLs and anchors-only
            if target.startswith(("http://", "https://", "mailto:", "#", "ftp:")):
                continue
            # Strip trailing anchor
            file_part = target.split("#")[0]
            if not file_part:
                continue
            resolved = (md.parent / file_part).resolve()
            if not resolved.exists():
                defects.append(
                    f"Broken link: '{target}' in {md.relative_to(REPO_ROOT)}"
                )

    return defects, False


# ── Check 7: Open-marker scan ─────────────────────────────────────────────────

EXPECTED_OPEN_SECTIONS = re.compile(
    r'(?:open\s+questions|tbd\s+list|tbds|open\s+items)',
    re.IGNORECASE
)


def check_open_markers() -> tuple[list[str], bool]:
    defects = []

    for md in all_md_files():
        text = md.read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        # Track whether we're inside a known TBD/open-questions section
        in_open_section = False
        for lineno, line in enumerate(lines, start=1):
            if line.startswith("#"):
                in_open_section = bool(EXPECTED_OPEN_SECTIONS.search(line))
            m = OPEN_PAT.search(line)
            if m:
                marker = m.group(1)
                excerpt = line.strip()[:80]
                location = f"{md.relative_to(REPO_ROOT)}:{lineno}"
                if in_open_section:
                    defects.append(f"[expected] {location} — {marker}: {excerpt}")
                else:
                    defects.append(f"{location} — {marker}: {excerpt}")

    return defects, False


# ── Check 8: Use-case-ID cross-reference ─────────────────────────────────────

def check_use_cases(inputs: dict) -> tuple[list[str], bool]:
    defects = []
    inconclusive = False

    uc_text = inputs.get("use_cases")
    if uc_text is None:
        return ["Inconclusive: use-case-descriptions.md is missing"], True

    uc_defined = set(UC_PAT.findall(uc_text))

    hld_all = "\n".join(
        v for k, v in inputs.items()
        if k not in ("srs", "vision", "use_cases", "colour_palette", "arch_principles")
        and v is not None
    )
    uc_cited = set(UC_PAT.findall(hld_all))

    for uc in sorted(uc_cited - uc_defined):
        defects.append(f"Broken trace: {uc} cited in HLD but not defined in use-case-descriptions.md")
    for uc in sorted(uc_defined - uc_cited):
        defects.append(f"Orphan: {uc} defined in use-case-descriptions.md but not cited in any HLD document")

    return defects, inconclusive


# ── Inventory (Check 9) ───────────────────────────────────────────────────────

def build_inventory(inputs: dict) -> dict[str, int]:
    inv: dict[str, int] = {}

    srs = inputs.get("srs", "")
    inv["Requirements (SRS)"] = len(set(REQ_PAT.findall(srs))) if srs else 0

    uc = inputs.get("use_cases", "")
    inv["Use cases"] = len(set(UC_PAT.findall(uc))) if uc else 0

    comp = inputs.get("components", "")
    component_names = _structural_components(comp) if comp else set()
    inv["Components"] = len(component_names)

    iface_names = _structural_interfaces(comp) if comp else set()
    inv["Interfaces"] = len(iface_names)

    hld = inputs.get("hld", "")
    decision_ids: set[int] = set()
    if hld:
        for m in DECISION_BARE.finditer(hld):
            decision_ids.add(int(m.group(1)[1:]))
    inv["Decisions"] = len(decision_ids)

    png_count = 0
    for d in PNG_DIRS:
        if d.exists():
            png_count += len(list(d.glob("*.png")))
    inv["Diagram PNGs"] = png_count

    return inv


# ── Report writer ─────────────────────────────────────────────────────────────

def render_check(num: int, name: str, defects: list[str], inconclusive: bool) -> str:
    expected_defects = [d for d in defects if d.startswith("[expected]")]
    real_defects = [d for d in defects if not d.startswith("[expected]")]

    if inconclusive:
        status = "INCONCLUSIVE"
    elif not real_defects:
        status = "PASS"
    else:
        status = f"{len(real_defects)} defect{'s' if len(real_defects) != 1 else ''}"

    lines = [f"## Check {num} — {name} [{status}]"]
    if real_defects:
        for d in real_defects:
            lines.append(f"- defect: {d}")
    if expected_defects:
        lines.append("")
        lines.append("_Expected open markers (in TBD/Open-questions sections):_")
        for d in expected_defects:
            lines.append(f"- {d[len('[expected] '):]}")
    if not defects and not inconclusive:
        lines.append("- No defects found.")
    return "\n".join(lines)


# ── Main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    # Load all inputs
    inputs: dict[str, str | None] = {}
    missing: list[str] = []
    for key, path in REQUIRED_INPUTS.items():
        text = read(path)
        inputs[key] = text
        if text is None:
            missing.append(str(path.relative_to(REPO_ROOT)))

    # Run checks
    results: list[tuple[int, str, list[str], bool]] = []

    d, inc = check_requirements(inputs)
    results.append((1, "Requirement-ID cross-reference", d, inc))

    d, inc = check_decisions(inputs)
    results.append((2, "Decision-ID coherence", d, inc))

    d, inc = check_components(inputs)
    results.append((3, "Component-name consistency", d, inc))

    d, inc = check_interfaces(inputs)
    results.append((4, "Interface-name consistency", d, inc))

    d, inc = check_diagrams()
    results.append((5, "Diagram embedding", d, inc))

    d, inc = check_links()
    results.append((6, "Markdown link integrity", d, inc))

    d, inc = check_open_markers()
    results.append((7, "Open-marker scan", d, inc))

    d, inc = check_use_cases(inputs)
    results.append((8, "Use-case-ID cross-reference", d, inc))

    # Inventory
    inv = build_inventory(inputs)

    # Totals
    total_defects = sum(
        len([x for x in d if not x.startswith("[expected]")])
        for _, _, d, inc in results
    )
    inconclusive_checks = sum(1 for _, _, _, inc in results if inc)

    # Build report
    report_lines = [
        "# Gate Review — Mechanical Report",
        f"Generated: {date.today().isoformat()}",
        "",
    ]

    if missing:
        report_lines.append("## Missing Input Files")
        for f in missing:
            report_lines.append(f"- `{f}` — checks that depend on this file are marked Inconclusive")
        report_lines.append("")

    # Inventory table
    report_lines.append("## Inventory")
    report_lines.append("")
    report_lines.append("| Artefact | Count |")
    report_lines.append("|---|---|")
    for name, count in inv.items():
        report_lines.append(f"| {name} | {count} |")
    report_lines.append("")

    # Check sections
    for num, name, defects, inc in results:
        report_lines.append(render_check(num, name, defects, inc))
        report_lines.append("")

    # Overall
    report_lines.append("## Overall")
    report_lines.append(
        f"Total defects: {total_defects} (across {len(results)} checks). "
        f"Inconclusive: {inconclusive_checks}."
    )

    report_text = "\n".join(report_lines) + "\n"

    # Write output
    out_path = REPO_ROOT / "docs" / "hld" / "gate-review" / "mechanical-report.md"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(report_text, encoding="utf-8")

    print(f"Report written to: {out_path.relative_to(REPO_ROOT)}")

    # Quick summary to stdout
    print(f"\nInventory:")
    for name, count in inv.items():
        print(f"  {name}: {count}")
    print(f"\nTotal defects: {total_defects} | Inconclusive checks: {inconclusive_checks}")


if __name__ == "__main__":
    main()
