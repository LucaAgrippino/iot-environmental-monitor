#!/bin/sh
# Build the console host simulator. Run from the repository root:  tools/console_simulator/build.sh
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
G="$R/firmware/gateway"
cc -std=gnu11 -Wall -O1 -DTEST -DSTM32L475xx -DBOARD_GATEWAY \
   -I"$G/application" -I"$G/application/console_service" -I"$G/application/console_service/core" \
   -I"$G/middleware" -I"$G/middleware/mqtt_client" -I"$G/middleware/wifi_task" -I"$G/drivers" \
   -I"$R/firmware/shared" -I"$R/tests/support" -I"$R/tests/mocks" -I"$R/vendor/coreMQTT/source/include" \
   "$G"/application/console_service/console_service.c "$G"/application/console_service/core/*.c \
   "$R/tests/support/freertos_mock.c" "$R/tools/console_simulator/main_host.c" "$R/tools/console_simulator/tty_host.c" \
   -o "${1:-/tmp/envmon-console-sim}"
