# Bug Log — SensorService + AlarmService

## sensor_service — wrong poll timer period constant

**File:** `firmware/field-device/application/sensor_service/sensor_service.c`
**Line:** 50
**Category:** wrong constant

**What the code does:**
Creates the periodic poll timer with a period of 200 ms, causing the sensor
acquisition cycle to run at 5 Hz.

**What it should do:**
The companion §9 specifies a 100 ms periodic tick, giving a 10 Hz acquisition
rate; the IIR filter alpha (0.1) is calibrated for this sample rate.

**Correct fix:**
```c
/* before */
#define SENSOR_POLL_INTERVAL_MS (200U)
/* after */
#define SENSOR_POLL_INTERVAL_MS (100U)
```

**How to find it with a debugger:**
1. Flash and open a SWO or UART terminal. Count the time between consecutive
   `[IT] Cycle N …` log lines — they arrive every ~200 ms instead of ~100 ms.
2. Alternatively, set a breakpoint at the top of `vSensorTask`'s inner loop
   (the `ulTaskNotifyTake` return) and use a hardware timer or the SWO
   timestamp to measure the inter-notification interval: it will be ~200 ms.
3. Inspect `SENSOR_POLL_INTERVAL_MS` in sensor_service.c and compare to the
   companion §9 specification of 100 ms.
4. A secondary symptom: the IIR filter appears more sluggish than expected —
   warming the sensor shows a time constant of ~2 s instead of ~1 s because
   the filter equation was designed for 10 Hz.

**Why it passes CI:**
The FreeRTOS mock's `xTimerCreateStatic` ignores all arguments and returns a
dummy handle; unit tests call `sensor_service_run_cycle()` directly and never
trigger the timer. No unit test measures the timer period or acquisition rate.
