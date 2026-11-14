#!/bin/bash
# usage: ocd.sh <openocd commands...>   (FD over SWD)
args=()
for c in "$@"; do args+=(-c "$c"); done
openocd -f interface/stlink-dap.cfg -c "adapter serial 066BFF303435554157025015" -c "transport select dapdirect_swd" -f target/stm32f4x.cfg "${args[@]}" 2>&1 | tail -2
