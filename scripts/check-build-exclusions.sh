#!/usr/bin/env bash
# scripts/check-build-exclusions.sh - fail if a CubeIDE build configuration would compile more (or
# fewer) than one main(). See check-build-exclusions.py for what it models.
# usage: scripts/check-build-exclusions.sh [--list] [PROJECT_DIR ...]
exec python3 "$(dirname "${BASH_SOURCE[0]}")/check-build-exclusions.py" "$@"
