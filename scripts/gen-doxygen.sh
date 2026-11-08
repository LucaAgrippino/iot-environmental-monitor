#!/usr/bin/env bash
# Generate the API documentation (HTML, with struct/UML, include and directory diagrams) for both firmwares.
#   scripts/gen-doxygen.sh [--with-sources] [gateway] [field-device]
# Default: the public API (headers only); the warning log is the REQ-NF-505 gate and is expected to be empty.
# --with-sources: also parse the .c files for call and caller graphs (warnings for file-local items are off).
# Needs doxygen and graphviz (dot). Output: generated-docs/doxygen/<board>/html/index.html, warnings: generated-docs/doxygen/<board>.warnings
set -euo pipefail
cd "$(dirname "$0")/.."
mode=api
if [ "${1:-}" = "--with-sources" ]; then mode=sources; shift; fi
mkdir -p generated-docs/doxygen
for board in ${@:-gateway field-device}; do
  cfg="docs/doxygen/Doxyfile.$board"
  if [ "$mode" = sources ]; then
    cfg="generated-docs/doxygen/Doxyfile.$board.sources"
    { cat "docs/doxygen/Doxyfile.$board"; cat docs/doxygen/Doxyfile.sources
      echo "OUTPUT_DIRECTORY = generated-docs/doxygen/$board-sources"; echo "WARN_LOGFILE = generated-docs/doxygen/$board-sources.warnings"; } > "$cfg"
  fi
  doxygen "$cfg"
  log="generated-docs/doxygen/$board.warnings"; [ "$mode" = sources ] && log="generated-docs/doxygen/$board-sources.warnings"
  echo "$board ($mode): $(wc -l < "$log") warnings; log $log"
done
