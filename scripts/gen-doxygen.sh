#!/usr/bin/env bash
# Generate the API documentation (HTML with struct/UML, include and call diagrams) for both firmwares.
# Needs doxygen and graphviz (dot). Output: build/doxygen/<board>/html/index.html, warnings: build/doxygen/<board>.warnings
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/doxygen
for board in ${@:-gateway field-device}; do
  doxygen "docs/doxygen/Doxyfile.$board"
  echo "$board: $(wc -l < "build/doxygen/$board.warnings") warnings, open build/doxygen/$board/html/index.html"
done
