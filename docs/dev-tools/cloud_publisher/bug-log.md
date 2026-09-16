# Bug Log — CloudPublisher (Gateway)

Bugs encountered while building this module, and how to find them. These are
mostly *design*-level rather than line-level: each is a case where the code did
exactly what it was written to do, and what it was written to do was wrong.

---

## Bug 1 — A wake bit that woke the task but was never cleared

**File:** `firmware/gateway/application/cloud_publisher/cloud_publisher.c`
**Function:** `prv_task_step()` / the `xTaskNotifyWait()` call
**Category:** missing state-clear → busy loop

**What the code did:**
`MQTT_CLIENT_WIFI_RECV_READY_BIT` (bit 5, owned by MqttClient) was added to
`CP_NOTIFY_ALL_BITS` as a **wake condition**, but not to `xTaskNotifyWait()`'s
**clear-on-exit mask**. `eSetBits` therefore woke the task, the bit stayed set,
and the next `xTaskNotifyWait()` returned immediately — forever.

**What it should do:**
A notification bit used as a wake condition must also appear in the clear-on-exit
mask, or it latches.

**Correct fix:**

    /* before */
    xTaskNotifyWait(0U, CP_NOTIFY_TIMER_BITS, &notify_value, timeout);

    /* after — same mask used for waking and for clearing */
    xTaskNotifyWait(0U, CP_NOTIFY_ALL_BITS, &notify_value, timeout);

**How to find it with a debugger:**
The signature is a **1 Hz tick logging four times a second** — a periodic task
running far faster than its period, with no timer misconfigured. Breakpoint
`xTaskNotifyWait()` and read the returned `notify_value` on consecutive calls:
the same bit set every time, with no intervening `eSetBits`, means it is never
being cleared. Cross-check by reading the task's notification value directly; it
should be zero immediately after a wait returns.

**Why it passes CI:**
The host FreeRTOS mock's `xTaskNotifyWait()` does not model the clear-on-exit
mask faithfully — it returns a value the test supplies and the test then moves
on. A busy loop needs a real scheduler and real elapsed time to be visible.
Pinned afterwards by **CP-T21** ("`wifi_recv_ready` bit cleared — no busy loop"),
which asserts the clear explicitly rather than only that the wake happened
(**CP-T20**).

---

## Bug 2 — Backoff finer than the clock that measures it

**File:** `firmware/gateway/application/cloud_publisher/cloud_publisher.c`
**Function:** `prv_maybe_reconnect()`
**Category:** wrong constant / resolution mismatch
**Tracked as:** CP-D13

**What the code did:**
The exponential reconnect backoff started at a 1 s minimum. The countdown that
implements it is decremented by the **1 Hz stats tick** — so a 1 s backoff means
"retry on the very next tick", which is indistinguishable from having no backoff
at all. Against a broker that is down, the gateway hammers it every second.

**What it should do:**
The minimum backoff must be at least twice the tick period for the countdown to
express anything. The shipped value is 2 s, doubling to a 60 s cap, reset on a
successful connect.

**Correct fix:**

    /* before */
    #define CP_RECONNECT_BACKOFF_MIN_S (1U)   /* == the tick period */
    /* after */
    #define CP_RECONNECT_BACKOFF_MIN_S (2U)   /* >= 2 ticks, or it cannot count */

**How to find it with a debugger:**
Log each reconnect attempt with a timestamp while the broker is down. Attempts
exactly 1 s apart that never lengthen mean the doubling is happening in a
variable that the countdown never gets to observe. Breakpoint the countdown
decrement and note it reaches zero on every tick regardless of the computed
backoff — the backoff is being *calculated* correctly and *consumed* instantly.

**Why it passes CI:**
Host tests advance time by calling the tick function directly, so any non-zero
backoff is observable — the test can make 1 s and 30 s look equally distinct.
The resolution limit only exists on a real 1 Hz timer. **CP-T18** (backoff does
not hammer every tick) and **CP-T25** (backoff is exponential and resets) pin the
behaviour, but neither can catch a minimum that is too close to the tick period.

---

## Bug 3 — The alarm path met its deadline in theory and missed it in practice

**File:** `firmware/gateway/application/cloud_publisher/cloud_publisher.c` (with MqttClient)
**Category:** design — synchronous path on a shared single task
**Tracked as:** CP-O6, fixed by MQTT-D13 + CP-D12

**What the code did:**
Alarms were published synchronously with QoS 1: the task transmitted, then
blocked waiting for the PUBACK. Measured on hardware, one QoS 1 publish is
~347 ms median — a single ISM43362 send alone is ~350 ms, about **70% of the
entire REQ-NF-113 500 ms budget**. Worse, a broker drop made the send loop run
to `MQTT_SEND_TIMEOUT_MS`, stalling the alarm path ~15 s.

**What it should do:**
Transmission and acknowledgement must be decoupled, and alarms must not queue
behind telemetry on the shared task.

**Correct fix (two parts):**

    /* MQTT-D13: publish transmits and returns; PUBACK reaped in process() */
    /* CP-D12:   drain the alarm queue before the telemetry queue */

**How to find it with a debugger:**
Timestamp alarm-raised and alarm-published either side of the publish call and
histogram the deltas over a few hundred alarms — a median comfortably inside
budget with a long tail is the shape to look for, not a single measurement. The
~15 s stall is found by dropping the broker mid-run and observing the task
parked inside the send retry loop rather than in its notification wait.

**Why it passes CI:**
There is no clock in a host test — a mocked publish returns instantly, so the
budget is trivially met. This requirement is only testable against real radio
latency, which is why `TC-HW-CP-012` exists.

**Honest outcome:** 240/240 alarms within budget, max 125 ms. But this is **not
a hard guarantee** — a co-in-flight publish on the single CloudPublisherTask can
still spike while one send costs 70% of the budget. A real guarantee needs
faster module I/O or a dedicated alarm path.
