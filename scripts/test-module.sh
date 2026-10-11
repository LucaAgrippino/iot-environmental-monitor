#!/usr/bin/env bash
# scripts/test-module.sh — Linux equivalent of test-module.ps1
#
# Runs Ceedling + cppcheck + clang-format for a single module.
#
# Usage:
#   ./scripts/test-module.sh -m <module>        # check mode (dry-run)
#   ./scripts/test-module.sh -m <module> -f      # fix mode (apply clang-format)
#
# Module name format:
#   gw-<module>    → Gateway   (firmware/gateway/...)
#   fd-<module>    → Field Device (firmware/field-device/...)
#   <module>       → Field Device (default, backward-compatible)
#
# Examples:
#   ./scripts/test-module.sh -m gw-circular_flash_log
#   ./scripts/test-module.sh -m fd-gpio -f
#   ./scripts/test-module.sh -m gpio            # same as fd-gpio

set -euo pipefail

# ── Defaults ──────────────────────────────────────────────────────────

FIX_MODE=false
MODULE=""
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLANG_FMT="clang-format-17"
SUPPRESSIONS="${REPO_ROOT}/cppcheck-suppressions.txt"

# ── Colour helpers ────────────────────────────────────────────────────

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass()  { echo -e "${GREEN}[PASS]${NC} $1"; }
fail()  { echo -e "${RED}[FAIL]${NC} $1"; }
info()  { echo -e "${YELLOW}[INFO]${NC} $1"; }

# ── Argument parsing ──────────────────────────────────────────────────

usage() {
    echo "Usage: $0 -m <module> [-f]"
    echo "  -m <module>  Module name (e.g. gw-spi_driver, fd-gpio, gpio)"
    echo "  -f           Fix mode: apply clang-format instead of dry-run"
    exit 1
}

while getopts "m:fh" opt; do
    case $opt in
        m) MODULE="$OPTARG" ;;
        f) FIX_MODE=true ;;
        h) usage ;;
        *) usage ;;
    esac
done

if [[ -z "$MODULE" ]]; then
    echo "Error: -m <module> is required."
    usage
fi

# ── Board and module name resolution ──────────────────────────────────

if [[ "$MODULE" == gw-* ]]; then
    BOARD="gateway"
    MOD_NAME="${MODULE#gw-}"
elif [[ "$MODULE" == fd-* ]]; then
    BOARD="field-device"
    MOD_NAME="${MODULE#fd-}"
else
    BOARD="field-device"
    MOD_NAME="$MODULE"
fi

# ── Find the module's source directory ────────────────────────────────

SRC_DIR=""
for layer in drivers middleware application; do
    candidate="${REPO_ROOT}/firmware/${BOARD}/${layer}/${MOD_NAME}"
    if [[ -d "$candidate" ]]; then
        SRC_DIR="$candidate"
        LAYER="$layer"
        break
    fi
done

if [[ -z "$SRC_DIR" ]]; then
    echo "Error: cannot find firmware/${BOARD}/{drivers,middleware,application}/${MOD_NAME}/"
    echo "Searched for: ${MOD_NAME} under ${BOARD}"
    exit 1
fi

info "Module: ${MOD_NAME}"
info "Board:  ${BOARD}"
info "Layer:  ${LAYER}"
info "Source: ${SRC_DIR}"
echo ""

# ── Track results ─────────────────────────────────────────────────────

CEEDLING_OK=false
CPPCHECK_OK=false
FORMAT_OK=false

# ── Step 1: Ceedling ──────────────────────────────────────────────────

info "Running Ceedling tests..."

# Generate version header if the script exists
VERSION_SCRIPT="${REPO_ROOT}/scripts/gen-version.py"
VERSION_IN="${REPO_ROOT}/firmware/shared/firmware_version.h.in"
VERSION_OUT="${REPO_ROOT}/firmware/shared/firmware_version.h"
if [[ -f "$VERSION_SCRIPT" && -f "$VERSION_IN" ]]; then
    python3 "$VERSION_SCRIPT" "$VERSION_IN" "$VERSION_OUT" 2>/dev/null || true
fi

# Gateway modules whose filename collides with a Field Device module run under
# their own Ceedling project (see tests/project_gateway.yml). Mirror what
# test-module.ps1 does: use it when it declares a :test_<module>: block.
ALT_PROJECT="${REPO_ROOT}/tests/project_gateway.yml"
if [[ "$BOARD" == "gateway" && -f "$ALT_PROJECT" ]] && \
   grep -qF ":test_${MOD_NAME}:" "$ALT_PROJECT"; then
    export CEEDLING_MAIN_PROJECT_FILE="project_gateway.yml"
    info "Using tests/project_gateway.yml"
fi

cd "${REPO_ROOT}/tests"
if ceedling "test:test_${MOD_NAME}" 2>&1; then
    CEEDLING_OK=true
    pass "Ceedling"
else
    fail "Ceedling"
fi
cd "${REPO_ROOT}"
echo ""

# ── Step 2: cppcheck ─────────────────────────────────────────────────

info "Running cppcheck..."

CPPCHECK_CMD=(
    cppcheck
    --enable=style,warning,performance
    --suppress=missingIncludeSystem
    --error-exitcode=1
)

if [[ -f "$SUPPRESSIONS" ]]; then
    CPPCHECK_CMD+=(--suppressions-list="$SUPPRESSIONS")
    CPPCHECK_CMD+=(--suppress=unmatchedSuppression)
fi

CPPCHECK_CMD+=("$SRC_DIR/")

if "${CPPCHECK_CMD[@]}" 2>&1; then
    CPPCHECK_OK=true
    pass "cppcheck"
else
    fail "cppcheck"
fi
echo ""

# ── Step 3: clang-format ─────────────────────────────────────────────

# Collect all .c and .h files in the source directory
mapfile -t SRC_FILES < <(find "$SRC_DIR" -maxdepth 2 \
    \( -name '*.c' -o -name '*.h' \) -type f 2>/dev/null)

# Also include the test file if it exists
TEST_DIR="${REPO_ROOT}/tests/${BOARD}/${LAYER}/${MOD_NAME}"
if [[ -d "$TEST_DIR" ]]; then
    while IFS= read -r f; do
        SRC_FILES+=("$f")
    done < <(find "$TEST_DIR" -maxdepth 1 \
        \( -name '*.c' -o -name '*.h' \) -type f 2>/dev/null)
fi

if [[ ${#SRC_FILES[@]} -eq 0 ]]; then
    info "No .c/.h files found — skipping clang-format."
    FORMAT_OK=true
else
    if $FIX_MODE; then
        info "Applying clang-format (fix mode)..."
        "$CLANG_FMT" -i "${SRC_FILES[@]}"
        FORMAT_OK=true
        pass "clang-format (applied)"
    else
        info "Checking clang-format (dry-run)..."
        if "$CLANG_FMT" --dry-run --Werror "${SRC_FILES[@]}" 2>&1; then
            FORMAT_OK=true
            pass "clang-format"
        else
            fail "clang-format (run with -f to fix)"
        fi
    fi
fi
echo ""

# ── Summary ───────────────────────────────────────────────────────────

echo "========================================"
echo "  Module: ${MODULE}"
echo "========================================"

ALL_PASSED=true
if $CEEDLING_OK;  then pass "Ceedling";      else fail "Ceedling";      ALL_PASSED=false; fi
if $CPPCHECK_OK;  then pass "cppcheck";      else fail "cppcheck";      ALL_PASSED=false; fi
if $FORMAT_OK;    then pass "clang-format";   else fail "clang-format";  ALL_PASSED=false; fi

echo ""
if $ALL_PASSED; then
    echo -e "${GREEN}ALL CHECKS PASSED${NC}"
    exit 0
else
    echo -e "${RED}SOME CHECKS FAILED${NC}"
    exit 1
fi
