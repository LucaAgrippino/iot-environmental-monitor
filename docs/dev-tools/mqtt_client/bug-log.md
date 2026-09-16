# Bug Log — MqttClient (Gateway)

No bug was intentionally planted. The real defects in this module are unusually
instructive because two of them are **conditional fixes** — the naive version of
each fix is itself a bug, and the second bug is strictly harder than the first.

---

## Bug 1 — The fix that broke every reconnect: unconditional fast-fail on send

**File:** `firmware/gateway/middleware/mqtt_client/mqtt_client.c`
**Function:** `prv_mbedtls_net_send()`
**Category:** wrong return value / state-dependent behaviour applied unconditionally
**Fixed in:** `6465fb9` / `40d28e9`

**What the code did:**
The original defect was real: on a dead socket, `net_send()` kept retrying until
`MQTT_SEND_TIMEOUT_MS`, stalling the caller ~15 s when the broker dropped. The
obvious fix — return a hard error as soon as the WiFi layer reports
`WIFI_ERR_SOCKET` — **is itself a bug**. During a TLS (re)handshake the socket
has only just been opened, and a transient `WIFI_ERR_SOCKET` is normal. Failing
hard there aborts *every* first connect and *every* reconnect.

**What it should do:**
Fast-fail only when the client believes it is connected. While a handshake is in
flight, a transient socket error is `MBEDTLS_ERR_SSL_WANT_WRITE` — retry.

**Correct fix:**

    /* before — stalls ~15 s on a dead peer */
    while (retries-- > 0) { ... }          /* to MQTT_SEND_TIMEOUT_MS */

    /* naive "fix" — aborts every reconnect */
    if (err == WIFI_ERR_SOCKET) { return MBEDTLS_ERR_NET_SEND_FAILED; }

    /* correct */
    if (err == WIFI_ERR_SOCKET)
    {
        if (inst->connected)
        {
            return MBEDTLS_ERR_NET_SEND_FAILED;  /* established: peer is gone */
        }
        return MBEDTLS_ERR_SSL_WANT_WRITE;       /* handshaking: retry */
    }

**How to find it with a debugger:**
Symptom of the naive fix: the board never connects at all, and the failure is
*immediate* rather than a timeout — that immediacy is the clue, because a real
network failure takes time. Breakpoint `prv_mbedtls_net_send()` and inspect
`inst->connected` when `WIFI_ERR_SOCKET` first appears: `false` during the
handshake means the fast-fail branch is being taken in the one state where it
must not be. Contrast with the established case, where `connected == true` and
the fast-fail is correct.

**Why it passes CI:**
Host tests drive `connect()` against a mocked transport that never returns a
transient `WIFI_ERR_SOCKET` on a freshly opened socket — that behaviour belongs
to the real ISM43362 module, whose socket takes a moment to become writable.
The mock's socket is writable the instant it is opened.

---

## Bug 2 — Socket slot leaked when the peer was already dead

**File:** `firmware/gateway/drivers/wifi_driver/wifi_driver.c` (surfaced by MqttClient)
**Function:** `wifi_close_socket()`
**Category:** missing state-clear / cleanup conditional on a remote operation
**Fixed in:** `38e02fa` — tracked as WIFI-O17, pinned by WIFI-T23

**What the code did:**
`wifi_close_socket()` issued the module's `P6=0` "stop client" command and only
released the local `socket_open[]` slot **if that command succeeded**. Against a
peer that had already gone away, `P6=0` fails — so the local slot stayed marked
in use forever. Every subsequent `wifitask_open_socket()` then returned `err=6`,
and reconnection was impossible until a power cycle.

**What it should do:**
Local bookkeeping must never be contingent on a remote operation succeeding. The
slot belongs to *us*; free it unconditionally, then make a best effort to tell
the module.

**Correct fix:**

    /* before */
    err = prv_at_stop_client(inst, sock);
    if (err == WIFI_ERR_OK)
    {
        inst->socket_open[sock] = false;   /* only on success */
    }
    return err;

    /* after */
    inst->socket_open[sock] = false;       /* unconditional, up front */
    return prv_at_stop_client(inst, sock); /* best effort */

**How to find it with a debugger:**
Drop the broker, wait for the disconnect, and watch the reconnect attempt fail
with `err=6` (no socket available). Inspect `inst->socket_open[]` — all four
slots `true` while no connection exists is conclusive. The tell in logs is that
the *first* reconnect after a clean disconnect works, but the first reconnect
after an abnormal drop does not: the difference is precisely whether `P6=0`
succeeded.

**Why it passes CI:**
The host mock's `close_socket` always succeeds, so the conditional is never
exercised on its failing branch. `MQTT-T19` ("multi-cycle disconnect/reconnect,
no socket exhaustion") passes against the mock for exactly that reason — which
is why `TC-HW-MQTT-011` exists: only real hardware proves the ISM43362 actually
frees the socket.

---

## Bug 3 — Handshake blamed on the radio when it was the compiler

**File:** `firmware/gateway/.cproject` (mbedtls-library folder optimisation)
**Category:** wrong build configuration / misattributed root cause
**Fixed in:** `d6609ff` — MQTT-O10

**What the code did:**
`mbedtls-library` was compiled at `-O0` like everything else in the Debug
configuration. A full mutual-auth TLS 1.2 handshake — RSA-2048 CertificateVerify
plus P-256 ECDHE, on an 80 MHz Cortex-M4 with no crypto accelerator — took
**~29 s**, against a 30 s deadline. Mosquitto 2.x independently drops clients
that have not sent CONNECT within 30 s, so the failure presented as a broker
rejection.

**What it should do:**
Crypto is the one place in a Debug build where `-O0` is not an acceptable cost.

**Correct fix:** a per-folder optimisation override to `-O2`, plus

    #define MBEDTLS_HAVE_ASM
    #define MBEDTLS_ECP_NIST_OPTIM

**How to find it with a debugger:**
Do not guess — measure. Read `DWT->CYCCNT` either side of
`mbedtls_ssl_handshake()`, then either side of the individual public-key
operations. Seeing tens of seconds inside `ecp_mul` / `rsa_private` while the
radio is idle settles it immediately. The trap is that "slow handshake on an
embedded WiFi module" *sounds* like an I/O problem, and the companion originally
recorded it as module-side latency.

**Why it passes CI:**
CI compiles and links; it never runs firmware and has no clock budget. The
firmware-build job is green regardless of how slow the result is.

**Correction carried forward:** the earlier claim that the ~30 s floor was
module-side latency was **wrong** — it was `-O0` crypto plus an unset `R2` in
the WiFi driver (WIFI-O16). Do not re-adopt that assumption.

---

## The bug worth rehearsing — QoS 1 publish treating a missing PUBACK as failure

**Not present in the shipped driver** (MQTT-T09b pins the correct behaviour),
but it is the natural mistake when `publish()` is made non-blocking under
MQTT-D13, and it is invisible to a mock.

**Category:** wrong return value / conflating transmission with acknowledgement

**What the buggy version does:** after MQTT-D13 made `publish()` transmit and
return, the PUBACK arrives later and is reaped in `mqtt_client_process()`. The
tempting shape is for `publish()` to report failure when no PUBACK is present
by the time it returns — which is *always*, since it no longer waits.

**What it should do:** `publish()` reports whether the packet was **transmitted**.
Acknowledgement is a separate, later event; a missing PUBACK at return time is
the normal case, not an error.

**Correct fix:**

    /* before */
    if (!inst->puback_seen[packet_id]) { return MQTT_CLIENT_ERR_PUBLISH; }
    /* after */
    return MQTT_CLIENT_ERR_OK;   /* transmitted; PUBACK reaped in process() */

**How to find it with a debugger:** every QoS 1 alarm reports failure while the
broker visibly receives all of them. Breakpoint `process()` and watch the PUBACK
arrive milliseconds *after* `publish()` already returned its error — the
ordering is the whole story. `get_stats()` shows `publishes_sent` incrementing
while `publishes_acked` lags by one, which is correct behaviour misread as a
fault.

**Why it passes CI:** a mock that acknowledges synchronously inside the publish
call makes the two designs indistinguishable. Only a real broker, with real
round-trip latency, separates "sent" from "acked".
