#!/usr/bin/env bash
# Rebuild the cloud guide: draw the diagrams (Graphviz), then print guide.html to ../Cloud-Guide.pdf with headless Chrome.
#   cloud/doc/src/build.sh
# Needs: python3, graphviz (dot), google-chrome, and the fonts Liberation Sans / Liberation Mono.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
command -v dot >/dev/null || { echo "graphviz (dot) not found" >&2; exit 1; }
chrome="$(command -v google-chrome || command -v chromium || command -v chromium-browser || true)"
[ -n "$chrome" ] || { echo "Chrome or Chromium not found" >&2; exit 1; }
python3 "$here/make_diagrams.py"
"$chrome" --headless=new --no-sandbox --disable-gpu --no-pdf-header-footer --virtual-time-budget=5000 \
    --print-to-pdf="$here/../Cloud-Guide.pdf" "file://$here/guide.html" >/dev/null 2>&1
ls -l "$here/../Cloud-Guide.pdf"
