# LCD UI Simulator

Host-side LVGL simulator for the IoT Environmental Monitor field-device display.
Builds inside a Docker container (Ubuntu 24.04 + SDL2 + CMake) and renders
natively on WSL2 via WSLg.

## Prerequisites

- **WSL2** with **WSLg** enabled (Windows 11 or Windows 10 21H2+).
  Run `echo $DISPLAY` inside WSL2 — it must return a value (e.g. `:0`).
- **Docker Desktop** with the WSL2 backend enabled.

## Build

```bash
cd simulator/lcd-ui
make build
```

This builds the Docker image (`lcd-sim-builder:latest`) and compiles the
simulator inside it, placing the binary at `build/lcd_sim`.

## Run

```bash
make run
```

Opens an SDL2 window (1600 × 960 physical pixels, 800 × 480 logical at 2×
upscale) on your WSLg display.  A dark background with the centred label
`LVGL 8.3.11 — simulator OK` confirms a working build.  Close the window
to exit.

## Shell into the container

```bash
make shell
```

Drops you into an interactive Bash session inside the builder container
with the repo mounted at `/work/repo`.  Useful for debugging CMake issues
or poking at LVGL internals.

## Troubleshooting

**Window does not appear / SDL error: cannot open display**

1. Check that `$DISPLAY` is set inside WSL2:
   ```bash
   echo $DISPLAY
   ```
   If empty, WSLg is not running.  Shut down WSL and restart:
   ```powershell
   wsl --shutdown
   ```
   Then reopen your WSL terminal.

2. Make sure you are running `make run` from WSL2 *outside* the container —
   the container does not have access to the WSLg socket.

**Docker: permission denied**

Make sure your user is in the `docker` group, or prefix with `sudo`.

**CMake cannot find SDL2**

The `Dockerfile` installs `libsdl2-dev` via apt.  If you are building
outside Docker, install it manually:
```bash
sudo apt-get install libsdl2-dev
```
