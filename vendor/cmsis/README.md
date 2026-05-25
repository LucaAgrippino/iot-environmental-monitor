# CMSIS headers (vendored)

This directory holds CMSIS device and core headers vendored from the
STM32CubeF4 and STM32CubeL4 firmware packages.

## Layout
- `core/` — ARM Cortex-M generic headers (core_cm4.h, cmsis_gcc.h, ...).
  Identical between F4 and L4 packs.
- `device-f4/` — ST device headers for STM32F4xx (used by Field Device).
- `device-l4/` — ST device headers for STM32L4xx (used by Gateway).

## Sources
- core: STM32CubeF4 V1.28.3 / Drivers/CMSIS/Include
- device-f4: STM32CubeF4 V1.28.3 / Drivers/CMSIS/Device/ST/STM32F4xx/Include
- device-l4: STM32CubeL4 V1.18.2 / Drivers/CMSIS/Device/ST/STM32L4xx/Include

## Licences
- ARM CMSIS Core: Apache-2.0 (see core/LICENSE.txt where present)
- ST device headers: Apache-2.0 (each file carries the licence header)

## Updating
Manual copy from a newer STM32Cube pack installation. Verify the
licence files come across too. Bump the version notes above.