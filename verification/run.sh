#!/usr/bin/env bash
# Run Robot Framework suites of the verification plan.
#   verification/run.sh 0            wave 0 (no hardware)
#   verification/run.sh 1 2 3        waves 1 to 3 (boards connected)
#   verification/run.sh suites/02_wave2_fd.robot --include REQ-LD-010   (anything else is passed to robot)
# Results: verification/build/results/<timestamp>/ (output.xml, log.html, report.html). The Python environment
# lives outside the repository: python3 -m venv ~/.venvs/envmon-verify && pip install robotframework pyserial pillow paho-mqtt
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
py="${VERIFY_PY:-$HOME/.venvs/envmon-verify/bin/python}"
stamp="$(date +%Y%m%d-%H%M%S)"
out="$root/verification/build/results/$stamp"
mkdir -p "$out" "$root/verification/build/captures"
suites=()
args=()
for a in "$@"; do
    case "$a" in
    [0-9]) suites+=("$(ls "$root"/verification/suites/0${a}_*.robot)") ;;
    *) args+=("$a") ;;
    esac
done
cd "$root"
exec "$py" -m robot --outputdir "$out" --pythonpath "$root/verification/libraries" \
    "${args[@]}" "${suites[@]}"
