# Bug Log — BarometerDriver and HumidityTempDriver

## barometer_driver — wrong constant in random walk delta

**File:** `firmware/field-device/drivers/barometer_driver/barometer_driver.c`
**Line:** 18
**Category:** wrong constant

**What the code does:**
`random_delta()` returns `(int32_t)(rand() % 4) - 2`, producing deltas in the range
[-2, +1], making the random walk asymmetric and biased toward the lower pressure bound.

**What it should do:**
Return `(int32_t)(rand() % 5) - 2`, producing a symmetric distribution in [-2, +2]
so the pressure walks without a directional bias.

**Correct fix:**
```c
/* before */
return (int32_t)(rand() % 4) - 2;
/* after */
return (int32_t)(rand() % 5) - 2;
```

**How to find it with a debugger:**
1. Flash the integration test (`test_simulated_sensors_main.c`) and let it run for
   5–10 minutes in the steady-state 1 Hz polling phase.
2. Observe the UART output: pressure values will drift progressively toward 3000
   (300.0 hPa) rather than fluctuating symmetrically around the initial 10132.
3. Set a software breakpoint on `random_delta()` in CubeIDE (Breakpoints view →
   "Add Breakpoint on Function Entry → random_delta").
4. Resume the target 20–30 times, logging the return value each time. You will
   observe that the maximum returned value is +1, never +2.
5. Compare against `rand() % 5` in the companion §3.2, which gives [0..4] before
   the subtraction of 2. The code uses 4 (modulus) instead of 5.

**Why it passes CI:**
All unit tests (T-BARO-02, T-BARO-06, T-BARO-07) only verify that readings stay
within [3000, 11000] after clamping — they do not check the statistical distribution
of deltas. With a maximum positive delta of +1, T-BARO-06 (force 10999, verify ≤ 11000)
still passes because 10999 + 1 = 11000 ≤ 11000. cppcheck has no rule for wrong modulus
operands.
