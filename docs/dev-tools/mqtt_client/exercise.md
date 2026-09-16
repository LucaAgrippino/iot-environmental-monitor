# Technical Exercise — MqttClient (Gateway)

## Brief (3 minutes)

A TLS stack sits on top of a WiFi module and calls a transport send hook. The
module reports `WIFI_ERR_SOCKET` in two completely different situations: the
peer has gone away and the connection is dead, or the socket was opened moments
ago and is not yet writable. The correct response is opposite in each case —
abort in the first, retry in the second — and the only thing distinguishing them
is the client's own connection state.

Implement the send hook. Getting this wrong in one direction stalls the caller
for the full send timeout every time a broker drops; getting it wrong in the
other aborts every single connection attempt before it can complete. Both
versions pass a mocked test suite.

## Given files

### `mqtt_net_exercise.h`

```c
#ifndef MQTT_NET_EXERCISE_H
#define MQTT_NET_EXERCISE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* mbedTLS transport contract (subset). */
#define MBEDTLS_ERR_SSL_WANT_WRITE     (-0x6880)
#define MBEDTLS_ERR_NET_SEND_FAILED    (-0x004E)
#define MBEDTLS_ERR_NET_INVALID_CONTEXT (-0x0045)

typedef enum
{
    WIFI_ERR_OK = 0,
    WIFI_ERR_SOCKET,  /**< Dead peer, OR a socket not yet writable. */
    WIFI_ERR_TIMEOUT,
    WIFI_ERR_PARAM
} wifi_err_t;

struct mqtt_inst
{
    uint8_t socket;
    bool connected; /**< True only once CONNACK has been accepted. */
};

/** Provided: one transmit attempt. Sets *sent to bytes accepted. */
wifi_err_t wifitask_send(uint8_t sock, const uint8_t *buf, size_t len, size_t *sent);

/**
 * @brief mbedTLS send hook.
 *
 * @param[in] ctx  struct mqtt_inst *.
 * @param[in] buf  Bytes to send.
 * @param[in] len  Byte count.
 * @return Bytes sent (> 0), or a negative MBEDTLS_ERR_* value.
 */
int prv_mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len);

#endif /* MQTT_NET_EXERCISE_H */
```

### `mqtt_net_exercise.c` (partial)

```c
#include "mqtt_net_exercise.h"

int prv_mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len)
{
    struct mqtt_inst *inst = (struct mqtt_inst *) ctx;

    /* TODO: context / argument guards. */

    /* TODO: one attempt — no retry loop. */

    /* TODO: map wifi_err_t to the mbedTLS contract. WIFI_ERR_SOCKET is the
     *       interesting one: its meaning depends on inst->connected. */

    return MBEDTLS_ERR_NET_SEND_FAILED;
}
```

## Questions

**Q1: Why can't `WIFI_ERR_SOCKET` simply be mapped to
`MBEDTLS_ERR_NET_SEND_FAILED`?**

Answer: Because the same error code covers two opposite situations. Once
`connected` is true, the session is established and a socket error genuinely
means the peer is gone — aborting is right, and is what removes the ~15 s stall
on a broker drop. But during the TLS handshake the socket has only just been
opened and may not be writable for a few milliseconds; a transient
`WIFI_ERR_SOCKET` there is normal. Mapping it to a hard failure aborts every
first connect and every reconnect — a strictly worse bug than the stall it was
meant to fix, because the stall was recoverable and this is not.

**Q2: Why must this be a single attempt rather than a retry loop?**

Answer: mbedTLS's own state machine already handles `WANT_WRITE` by calling the
hook again on the next tick — that is the mechanism the ticked connect
(`connect_step()`) is built on. A retry loop inside the hook duplicates that,
and it does so *blocking*, which defeats the entire purpose: the caller's tick
can no longer bound how long one step takes. The retry loop is what produced the
original ~15 s stall.

**Q3: `wifitask_send()` returns `WIFI_ERR_OK` with `*sent` less than `len`.
What should the hook return, and why does it matter here specifically?**

Answer: Return `sent` — the actual byte count. mbedTLS treats any positive
return as a partial write and re-offers the remainder, so a short write is
normal and must not be reported as an error. It matters here because the WiFi
module has a bounded internal buffer and genuinely does accept less than a full
TLS record during a handshake flight. Returning `len` regardless would silently
drop bytes and corrupt the record stream — a failure that looks like a
protocol-level fault far from its cause.

## Model solution

```c
#include "mqtt_net_exercise.h"

int prv_mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len)
{
    struct mqtt_inst *inst = (struct mqtt_inst *) ctx;
    size_t sent = 0U;
    wifi_err_t err;

    if (inst == NULL)
    {
        return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    }
    if ((buf == NULL) || (len == 0U))
    {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    /* One attempt. mbedTLS re-drives us via WANT_WRITE; a loop here would
     * block the caller's tick and defeat the ticked connect state machine. */
    err = wifitask_send(inst->socket, (const uint8_t *) buf, len, &sent);

    switch (err)
    {
        case WIFI_ERR_OK:
            if (sent == 0U)
            {
                return MBEDTLS_ERR_SSL_WANT_WRITE; /* Nothing taken; retry. */
            }
            return (int) sent; /* May be < len — mbedTLS re-offers the rest. */

        case WIFI_ERR_TIMEOUT:
            return MBEDTLS_ERR_SSL_WANT_WRITE;

        case WIFI_ERR_SOCKET:
            /* The state-dependent case. Established: the peer is gone.
             * Handshaking: the socket is simply not writable yet. */
            if (inst->connected)
            {
                return MBEDTLS_ERR_NET_SEND_FAILED;
            }
            return MBEDTLS_ERR_SSL_WANT_WRITE;

        case WIFI_ERR_PARAM:
        default:
            return MBEDTLS_ERR_NET_SEND_FAILED;
    }
}
```

## Marking guide

**Must have:**

- `WIFI_ERR_SOCKET` handled **conditionally** on `inst->connected`.
- Exactly one transmit attempt — no retry loop.
- Returns the actual `sent` count, not `len`.
- NULL context guard before dereferencing `ctx`.
- Distinguishes "retry" (`WANT_WRITE`) from "fail" (`NET_SEND_FAILED`).

**Good to have:**

- Treats `sent == 0` with `WIFI_ERR_OK` as `WANT_WRITE` rather than returning 0
  (a zero return is ambiguous in the mbedTLS contract).
- Maps `WIFI_ERR_TIMEOUT` to `WANT_WRITE`, not failure.
- Can explain why the conditional is safe: `connected` is set only after CONNACK,
  so the handshake window is exactly the period where retry is correct.
- Notes that the symmetric `net_recv` hook has the same structure, and that the
  equivalent `WANT_READ` mapping is what powers `connect_step()`.

**Red flags:**

- Unconditional fast-fail on `WIFI_ERR_SOCKET` — aborts every connection.
- A `while (retries--)` loop — reintroduces the stall this design removed.
- Returning `len` on a short write.
- Returning 0 to mean "try again" — mbedTLS does not read it that way.
- Setting `inst->connected` inside the send hook; connection state belongs to
  the CONNACK path, and touching it here creates a feedback loop between the
  transport and the session layer.
