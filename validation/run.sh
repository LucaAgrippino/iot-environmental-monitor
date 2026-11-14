#!/usr/bin/env bash
# Run the validation suites (docs/test/validation-plan.md).
#   VAL_PASS=... validation/run.sh api            Remote Operator scenarios through the cloud API (about 8 minutes, restarts the Gateway)
#   VAL_PASS=... validation/run.sh web            Remote Operator scenarios in a headless browser (about 1 minute)
#   VAL_PASS=... validation/run.sh suites/10_remote_operator_api.robot --test "UC-14*"   (anything else goes to robot)
# VAL_PASS is the password of a cloud account with the admin role (iotcloud-admin add-user); it is never written to a file.
# Environment: python3 -m venv ~/.venvs/envmon-validate && pip install robotframework playwright requests paho-mqtt pyserial
#              && playwright install chromium. Run one suite at a time: both drive the same Gateway.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
py="${VAL_PY:-$HOME/.venvs/envmon-validate/bin/python}"
: "${VAL_PASS:?set VAL_PASS to the validation account password}"
suites=()
args=()
for a in "$@"; do
    case "$a" in
    api) suites+=("$root/validation/suites/10_remote_operator_api.robot") ;;
    web) suites+=("$root/validation/suites/20_remote_operator_web.robot") ;;
    *) args+=("$a") ;;
    esac
done
out="$root/validation/build/results/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
cd "$root/validation"
exec "$py" -m robot --outputdir "$out" --pythonpath libraries "${args[@]}" "${suites[@]}"
