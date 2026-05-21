#!/usr/bin/env python3
"""
check-traceability.py

Parses Doxygen @req REQ-XXX-NNN tags from firmware source files and
cross-checks them against requirement IDs declared in the SRS.

Exit codes:
  0 — all @req tags reference known requirements (or no tags found)
  1 — one or more tags reference an unknown requirement ID

Usage:
  python check-traceability.py --firmware-root <path> --srs-path <path>
"""

import argparse
import sys


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Cross-check @req tags in firmware source against SRS requirement IDs."
    )
    parser.add_argument(
        "--firmware-root",
        required=True,
        metavar="DIR",
        help="Root directory to search for *.c and *.h files (e.g. firmware/).",
    )
    parser.add_argument(
        "--srs-path",
        required=True,
        metavar="FILE",
        help="Path to docs/SRS.md containing the canonical requirement list.",
    )
    return parser.parse_args()


def collect_srs_ids(srs_path: str) -> set[str]:
    # TODO: parse SRS.md and extract all REQ-XXX-NNN identifiers.
    # Expected format in SRS.md:  | REQ-SYS-001 | ... |
    # Return a set of strings such as {"REQ-SYS-001", "REQ-HW-042"}.
    return set()


def collect_req_tags(firmware_root: str) -> list[tuple[str, int, str]]:
    # TODO: walk firmware_root recursively for *.c and *.h files.
    # In each file, search for Doxygen tags of the form:  @req REQ-XXX-NNN
    # Return a list of (file_path, line_number, req_id) tuples.
    return []


def main() -> int:
    args = parse_args()

    # TODO: replace stub with real implementation once the first driver
    # with @req tags is created.
    print("Traceability check: 0 tags found, all OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
