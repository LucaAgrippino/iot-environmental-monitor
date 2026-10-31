#!/usr/bin/env python3
"""Check that every CubeIDE build configuration compiles exactly one main().

Every firmware image in this project is one harness (or the production main) chosen by the
`excluding=` lists of the source entries in `.cproject`. A new harness folder that is not named in a
list is compiled in next to the existing one and the link fails with two main() functions (or, the
other way round, a configuration compiles none). This script reads `.cproject` and `.project`,
resolves the source entries (including the linked folders), applies the exclusion patterns, and
counts the main() definitions among the C files that remain.

usage: check-build-exclusions.py [--list] [--config NAME ...] [PROJECT_DIR ...]
       PROJECT_DIR defaults to firmware/gateway and firmware/field-device.
       --list         also print the main() files of every configuration.
       --config NAME  check only the configuration(s) with that name (default: all of them). CI
                      uses --config Debug, the only configuration it builds.

exit status: 0 when every configuration compiles exactly one main(), 1 otherwise, 2 on a
project it cannot read.
"""

import fnmatch
import os
import re
import sys
import urllib.parse
import xml.etree.ElementTree as ET

MAIN_RE = re.compile(r"^\s*(?:int|void)\s+main\s*\(", re.MULTILINE)
VAR_RE = re.compile(r"\$\{PARENT-(\d+)-PROJECT_LOC\}")


def linked_folders(project_dir):
    """Map the name of each linked folder in .project to its real directory."""
    links = {}
    tree = ET.parse(os.path.join(project_dir, ".project"))
    for link in tree.getroot().iter("link"):
        name = link.findtext("name")
        uri = urllib.parse.unquote(link.findtext("locationURI") or "")
        match = VAR_RE.match(uri)
        if name and match:
            base = project_dir
            for _ in range(int(match.group(1))):
                base = os.path.dirname(base)
            links[name] = os.path.normpath(base + uri[match.end():])
    return links


def is_excluded(rel_path, patterns):
    for pattern in patterns:
        if rel_path == pattern or rel_path.startswith(pattern + "/"):
            return True
        if any(ch in pattern for ch in "*?[") and fnmatch.fnmatch(rel_path, pattern):
            return True
    return False


def compiled_mains(project_dir, links, entries):
    """Return the main() files that the given source entries compile, relative to project_dir."""
    mains = []
    for name, excluding in entries:
        root = links.get(name, os.path.join(project_dir, name))
        if not os.path.isdir(root):
            continue
        patterns = [p for p in excluding.split("|") if p]
        for directory, dirnames, filenames in os.walk(root):
            rel_dir = os.path.relpath(directory, root).replace("\\", "/")
            prefix = "" if rel_dir == "." else rel_dir + "/"
            dirnames[:] = [d for d in dirnames if not is_excluded(prefix + d, patterns)]
            for filename in filenames:
                if not filename.endswith(".c") or is_excluded(prefix + filename, patterns):
                    continue
                path = os.path.join(directory, filename)
                with open(path, encoding="utf-8", errors="replace") as handle:
                    if MAIN_RE.search(handle.read()):
                        mains.append(os.path.relpath(path, project_dir).replace("\\", "/"))
    return sorted(mains)


def check_project(project_dir, show_list, only):
    links = linked_folders(project_dir)
    root = ET.parse(os.path.join(project_dir, ".cproject")).getroot()
    ok = True
    found = False
    for config in root.iter("configuration"):
        source_entries = config.find("sourceEntries")
        if source_entries is None:
            continue
        if only and config.get("name") not in only:
            continue
        found = True
        entries = [(e.get("name"), e.get("excluding", "")) for e in source_entries.iter("entry")
                   if e.get("kind") == "sourcePath"]
        mains = compiled_mains(project_dir, links, entries)
        verdict = "ok" if len(mains) == 1 else "FAIL"
        print("%s  %s / %s: %d main()" % (verdict, os.path.basename(project_dir),
                                          config.get("name"), len(mains)))
        if len(mains) != 1:
            ok = False
            for path in mains:
                print("      " + path)
            if not mains:
                print("      (none: the image has no entry point)")
        elif show_list:
            print("      " + mains[0])
    if not found:
        print("FAIL  %s: no matching build configuration with source entries" % project_dir)
        return None
    return ok


def main(argv):
    show_list = False
    only = []
    dirs = []
    args = iter(argv)
    for arg in args:
        if arg == "--list":
            show_list = True
        elif arg == "--config":
            only.append(next(args, ""))
        elif arg.startswith("--"):
            print("unknown option " + arg)
            return 2
        else:
            dirs.append(arg)
    if not dirs:
        repo = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
        dirs = [os.path.join(repo, "firmware", "gateway"),
                os.path.join(repo, "firmware", "field-device")]
    status = 0
    for project_dir in dirs:
        project_dir = os.path.abspath(project_dir)
        try:
            result = check_project(project_dir, show_list, only)
        except (OSError, ET.ParseError) as err:
            print("cannot read %s: %s" % (project_dir, err))
            return 2
        if result is None:
            return 2
        if not result:
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
