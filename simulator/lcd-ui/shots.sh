#!/usr/bin/env bash
# Render every screen and state of the design pack with the simulator, headless.
#
# usage: shots.sh <lcd_sim binary> <output dir>
#
# Writes PPM files (and PNG files when python3 with Pillow is available). The coordinates are logical
# pixels: tab i is at x = 80 + 160 i, y = 32; the Config screen's first field input is at
# (200, 160), its keypad keys are 89 x 54 px starting at (498, 172).
set -euo pipefail
sim="${1:?usage: shots.sh <lcd_sim> <outdir>}"
out="${2:?usage: shots.sh <lcd_sim> <outdir>}"
mkdir -p "$out"

TAB_SENSORS="tap 80 32"; TAB_TREND="tap 240 32"; TAB_STATUS="tap 400 32"; TAB_ALARMS="tap 560 32"
TAB_CONFIG="tap 720 32"
FIELD0="tap 200 160"
K1="tap 542 319"; K2="tap 640 319"; K3="tap 732 319"
K4="tap 542 259"; K5="tap 640 259"; K6="tap 732 259"
KDOT="tap 542 379"; K0="tap 640 379"; KDEL="tap 732 379"
APPLY="tap 600 426"; CANCEL="tap 200 426"

run() { # name, script
    SDL_VIDEODRIVER=dummy "$sim" --script "$2; shot $out/$1.ppm" 2> /dev/null
}

run splash-boot-28    "splash show; boot 28 Initialising LCD driver..."
run splash-boot-62    "splash show; boot 62 Initialising sensor bus..."
run splash-ready      "splash show; boot 100 System ready"
run sensors-normal    "warp 310"
run sensors-waiting   "scenario waiting; warp 2"
run sensors-stale     "scenario stale; warp 60"
run sensors-error     "scenario error; warp 30"
run sensors-alarm     "scenario alarm; warp 30"
run trend-accumulating "warp 120; $TAB_TREND; warp 1"
run trend-full-gap    "scenario gap; warp 320; $TAB_TREND; warp 1"
run status-healthy    "warp 5; $TAB_STATUS; warp 2"
run status-degraded   "scenario degraded; warp 15; $TAB_STATUS; warp 2"
run alarms-empty      "warp 5; $TAB_ALARMS; warp 2"
run alarms-active     "scenario alarm; warp 5; $TAB_ALARMS; warp 2"
run config-idle       "warp 5; $TAB_CONFIG; warp 1"
run config-editing    "warp 5; $TAB_CONFIG; $FIELD0; $KDEL; $KDEL; $KDEL; $KDEL; $K3; $K5; $KDOT; $K0; warp 1"
run config-error      "warp 5; $TAB_CONFIG; $FIELD0; $KDEL; $KDEL; $KDEL; $KDEL; $K5; $KDOT; $K0; warp 1"
run config-modal      "warp 5; $TAB_CONFIG; $FIELD0; $KDEL; $KDEL; $KDEL; $KDEL; $K3; $K5; $KDOT; $K0; $APPLY; warp 1"

if python3 -c "import PIL" 2> /dev/null; then
    python3 - "$out" <<'PY'
import glob, sys
from PIL import Image
for f in glob.glob(sys.argv[1] + "/*.ppm"):
    Image.open(f).save(f[:-4] + ".png")
PY
fi
echo "wrote $(ls "$out" | grep -c ppm) screenshots to $out"
