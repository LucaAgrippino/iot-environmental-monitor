# Bug Log — WifiTask (Gateway)

No bug was intentionally planted. This module produced an unusually rich crop
of **real** defects — three in one hardware session, each masking the next —
and they are far better interview material than anything contrived, because
every one of them passed the full host suite.

The unifying lesson: **WifiTask's host tests mock FreeRTOS and the WiFi
driver, so they verify *logic*. All three bugs were timing or byte-stream
semantics.** A green suite said nothing about either.

---

## Bug 1 — Background recv never woke: up to 30 s before the first poll

**File:** `firmware/gateway/middleware/wifi_task/wifi_task.c`
**Function:** `wifitask_try_recv()` / `prv_wifitask_step()`
**Category:** missing wake-up / race between a queue block and an arm flag
**Fixed in:** `7a66508`

**What the code did:**
`prv_wifitask_step()` blocks in `xQueueReceive(request_queue, ...)` with a
timeout of `WIFI_LIVENESS_CHECK_PERIOD_MS` (30 s), and only checks the *arm*
queue at the top of a step. `wifitask_try_recv()` armed a slot but sent
nothing to `request_queue`. With WifiTask idle, the arm therefore sat unseen
until the 30 s liveness timeout expired.

**What it should do:**
Arming a background recv must also wake the task, or the arm is invisible for
up to a full liveness period.

**Correct fix:**

    /* before */
    slot->state = RECV_SLOT_ARMED;
    return WIFITASK_RECV_PENDING;

    /* after */
    slot->state = RECV_SLOT_ARMED;
    /* Wake the step: a NULL request is a kick, not work. */
    (void) xQueueSend(inst->request_queue, &(wifitask_request_t *){NULL}, 0);
    return WIFITASK_RECV_PENDING;

**How to find it with a debugger:**
The symptom is a TLS handshake that times out with no bytes read. Put a
breakpoint on the `wifi_recv()` call inside the background poll and note it is
never reached within the handshake deadline. A packet capture is decisive: the
module ACKs the entire server flight at **+7 ms** while the firmware reads
nothing for 30 s — proving the bytes arrived and the *reader* was asleep, not
that the peer was slow. Then breakpoint `prv_wifitask_step()` and observe it
parked in `xQueueReceive` with the arm flag already set.

**Why it passes CI:**
The host `freertos_mock.c` `xQueueReceive` returns immediately; there is no
30 s block to observe. The arm-then-step-then-pickup sequence the unit tests
drive (WIFITASK-T18) calls `step()` explicitly, so the missing wake is
structurally invisible. Pinned afterwards by **WIFITASK-T21** ("`try_recv()`
from IDLE kicks the request queue") and **T22** ("`step()` ignores a NULL kick
without dispatching").

---

## Bug 2 — Pickup restarted the payload from offset 0 every time

**File:** `firmware/gateway/middleware/wifi_task/wifi_task.c`
**Function:** recv-slot pickup path
**Category:** missing consumption cursor / stream treated as a datagram
**Fixed in:** `7a66508`

**What the code did:**
Pickup copied `min(scratch_len, buf_len)` bytes **from offset 0** and left the
slot `DONE`. It also re-armed on the first pickup, so a fresh background
attempt could overwrite `scratch` while the previous payload was still being
consumed.

**What it should do:**
TLS is a byte stream. mbedTLS reads a 5-byte record header, then the body, as
two separate calls. Each pickup must resume where the last one stopped, and
the buffer must not be recycled until fully consumed.

**Correct fix:**

    /* before */
    copy = MIN(slot->scratch_len, buf_len);
    memcpy(buf, slot->scratch, copy);
    slot->state = RECV_SLOT_DONE;

    /* after */
    copy = MIN(slot->scratch_len - slot->scratch_off, buf_len);
    memcpy(buf, &slot->scratch[slot->scratch_off], copy);
    slot->scratch_off += copy;
    if (slot->scratch_off == slot->scratch_len)
    {
        slot->state = RECV_SLOT_DONE;   /* fully consumed — re-arm allowed */
    }

**How to find it with a debugger:**
mbedTLS fails the handshake with a malformed-record error. Breakpoint the
pickup and log `(scratch_off, scratch_len, copy)` on each call: the giveaway
is `scratch_off` stuck at 0 across consecutive calls while `copy` is 5, 5, 5 —
the header being delivered repeatedly and the body never. Cross-check by
dumping the first bytes handed to mbedTLS on two successive reads; identical
bytes mean the cursor is missing, not that the peer resent.

**Why it passes CI:**
The host tests feed one whole synthetic frame and pick it up once. A cursor is
indistinguishable from no cursor when there is exactly one read. Pinned
afterwards by **WIFITASK-T23** ("partial pickups are consecutive, never
duplicated") and **T24** ("no re-arm over an unconsumed payload").

---

## Bug 3 — Empty background polls starved the owner's sends

**File:** `firmware/gateway/middleware/wifi_task/wifi_task.c`
**Category:** wrong constant / head-of-line blocking
**Fixed in:** `7a66508` (with the root cause one layer down in WifiDriver, WIFI-O16)

**What the code did:**
Every background recv attempt used `WIFITASK_WIFI_RESP_TIMEOUT_MS` (5000 ms).
Because WifiTask is single-threaded, a 5 s *empty* poll blocked the same
owner's queued `wifitask_send()` calls for those 5 s. The handshake's
four-send client flight drifted to 19 s+ between records.

**What it should do:**
A speculative background poll must be short. Only a caller genuinely waiting
on data should pay a long timeout.

**Correct fix:**

    /* before */
    err = wifi_recv(sock, buf, len, WIFITASK_WIFI_RESP_TIMEOUT_MS);   /* 5000 */
    /* after */
    err = wifi_recv(sock, buf, len, WIFITASK_RECV_POLL_TIMEOUT_MS);   /* 200 */

...plus: an empty attempt must not wake the owner. Before that half of the
fix, the owner re-armed on every wake and the pair spun at 4 Hz — visible as
CloudPublisher's 1 Hz stats tick logging four times a second.

**How to find it with a debugger:**
Timestamp each `wifi_send()` entry/exit. The pattern is sends spaced ~5 s
apart with the gaps sitting inside WifiTask's recv, not inside the send.
The 4 Hz log spin is the other tell — a 1 Hz tick printing 4×/s means
something is waking the task that should not be.

**Why it passes CI:**
Timeout values are arguments to a mocked `wifi_recv()`; the host mock returns
instantly regardless. No host test can distinguish 200 ms from 5000 ms. Pinned
afterwards by **WIFITASK-T25** ("an empty attempt is short and does not wake
the owner").

---

## Bug 4 — Recv slot not reset across a socket reopen

**File:** `firmware/gateway/middleware/wifi_task/wifi_task.c`
**Category:** missing state-clear
**Fixed in:** `b1322d3`

**What the code did:**
`wifitask_open_socket()` / `wifitask_close_socket()` left the per-slot recv
state untouched. After a broker drop, the slot still held `POLL_ERROR` from
the *dead* connection, so the first read on the newly opened socket returned
an error that had nothing to do with it — surfacing as mbedTLS `-0x6800`.

**What it should do:**
Socket identity changes on reopen; every byte of state tied to the old socket
must be cleared.

**Correct fix:**

    /* after — in both open and close paths */
    prv_recv_slot_reset(&inst->recv_slots[sock]);

**How to find it with a debugger:**
Only reachable by dropping the broker mid-session. Breakpoint after
`open_socket()` returns OK and inspect `recv_slots[sock].state` — finding
`POLL_ERROR` on a socket that has never been read is conclusive. The tell in
logs is a handshake that fails *immediately* on reconnect, with no round trip
on the wire.

**Why it passes CI:**
Host tests never close and reopen a socket within one test — each starts from
`reset_for_test()`. Pinned afterwards by **WIFITASK-T26**.

---

## Related: the sibling driver bug this exposed (WIFI-O17)

Not a WifiTask defect, but found by the same broker-down run and required for
reconnect to work: `wifi_close_socket()` leaked the local `socket_open[]` slot
when the module's `P6=0` "stop client" failed on a dead peer, so the next
`wifitask_open_socket()` returned `err=6`. Fix (`38e02fa`) clears the slot up
front, unconditionally — the local bookkeeping must not depend on the remote
peer still being alive.
