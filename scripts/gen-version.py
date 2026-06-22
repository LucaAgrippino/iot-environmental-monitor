#!/usr/bin/env python3
"""Generate firmware_version.h from firmware_version.h.in and git describe.

Usage: python3 gen-version.py <template_path> <output_path>

Idempotent: skips write if content unchanged. Tolerates untagged repos
(degrades to v0.0.0+<sha>) and missing git binary (writes
v0.0.0+untagged). All timestamps in UTC.
"""

import datetime
import pathlib
import re
import subprocess
import sys


def get_git_describe():
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty", "--match=v*"],
            text=True, stderr=subprocess.DEVNULL).strip()
        return out
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def parse(describe):
    if describe is None:
        return {"major": 0, "minor": 0, "patch": 0,
                "git": "untagged", "string": "v0.0.0+untagged"}

    m = re.match(r"^v(\d+)\.(\d+)\.(\d+)(?:-(\d+)-g([0-9a-f]+))?(-dirty)?$",
                 describe)
    if m is None:
        m = re.match(r"^([0-9a-f]+)(-dirty)?$", describe)
        if m is None:
            return {"major": 0, "minor": 0, "patch": 0,
                    "git": describe, "string": f"v0.0.0+{describe}"}
        sha, dirty = m.groups()
        suffix = "-dirty" if dirty else ""
        return {"major": 0, "minor": 0, "patch": 0,
                "git": f"{sha}{suffix}", "string": f"v0.0.0+{sha}{suffix}"}

    major, minor, patch, commits, sha, dirty = m.groups()
    dirty_suffix = "-dirty" if dirty else ""

    if commits is None:
        return {"major": int(major), "minor": int(minor), "patch": int(patch),
                "git": f"clean{dirty_suffix}",
                "string": f"v{major}.{minor}.{patch}{dirty_suffix}"}

    return {"major": int(major), "minor": int(minor), "patch": int(patch),
            "git": f"{sha}{dirty_suffix}",
            "string": f"v{major}.{minor}.{patch}+{sha}{dirty_suffix}"}


def main(template_path, output_path):
    v = parse(get_git_describe())
    now = datetime.datetime.now(datetime.timezone.utc)
    text = pathlib.Path(template_path).read_text()
    substitutions = {
        "@FW_MAJOR@":  str(v["major"]),
        "@FW_MINOR@":  str(v["minor"]),
        "@FW_PATCH@":  str(v["patch"]),
        "@FW_GIT@":    v["git"],
        "@FW_STRING@": v["string"],
        "@FW_DATE@":   now.strftime("%Y-%m-%d"),
        "@FW_TIME@":   now.strftime("%H:%M:%S"),
    }
    for k, val in substitutions.items():
        text = text.replace(k, val)

    out = pathlib.Path(output_path)
    if out.exists() and out.read_text() == text:
        return
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text)
    print(f"[gen-version] wrote {output_path}: {v['string']}",
          file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <template> <output>", file=sys.stderr)
        sys.exit(2)
    main(sys.argv[1], sys.argv[2])
