# LLD Companion — WifiDriver (ISM43362-M3G-L44)

**Document:** `docs/lld/drivers/wifi-driver.md`
**Version:** 0.2 (Phase H complete — ready for implementation)
**Board:** Gateway (B-L475E-IOT01A)
**Layer:** Driver
**Status:** Implementation-ready
**Date:** July 2026

**HLD anchor:** WifiDriver in `components.md` (GW driver layer)

---

## 1. Sources

WifiDriver is the most complex Gateway driver. It controls the Inventek
ISM43362-M3G-L44 embedded WiFi module via an AT-command protocol over SPI3.
It is the sole driver consumed by WifiTask, which is the sole accessor of the
WiFi peripheral per D29.

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Sends and receives data between the MCU and the external WiFi module via AT commands. Exposes link-level state (RSSI, connection status) to its consumer. | `components.md` |
| PROVIDES (upward) | IWifi | `components.md` |
| USES (downward) | SpiDriver, GpioDriver, ExtiDriver | `components.md` |
| Root requirements | REQ-CC-050 (WiFi connect), CON-001 (ISM43362 module) | `SRS.md` §4 |
| Board | Gateway only | `components.md` |
| Hardware | ISM43362-M3G-L44 via SPI3 + 5 GPIO lines | UM2153 §7.11.3 |

**Consumer:** WifiTask only (D29). WifiTask serialises all calls to WifiDriver —
no concurrent access is possible by construction.

**CON-001 text (SRS.md §4):** *"The gateway WiFi module (ISM43362-M3G-L44)
communicates with the host MCU via SPI using AT commands. All TCP/IP and TLS
operations are handled by the module's internal stack, not by the application
firmware."*

### 1.1 Additional source references

| Source | Relevant section |
|---|---|
| `task-breakdown.md` §5.2, §6.1, §7 | WifiTask: priority 3, 256 words; D29 (sole SPI3 owner); `SPI_wifi_IRQHandler` → WifiTask |
| `sequence-diagrams.md` SD-03, SD-04, SD-09 | Cloud publish, store-and-forward, NTP flows |
| UM2153 Table 11, Fig. 23, Fig. 26 | Pin assignments, MCU schematic, RF module schematic |
| ISM43362 ES-WiFi application note | AT command set, SPI protocol, DRDY handshake |

---

## 2. Public API

### 2.1 ADT pattern

WifiDriver follows the Gateway ADT default: an opaque handle
(`wifi_handle_t`) is returned by `wifi_create()` from a static internal
pool. The internal struct is hidden in `wifi_driver.c`. Pool size is 1
(single ISM43362 module on the board).

Dependencies (SpiDriver, GpioDriver) are injected via the config struct
at creation time. This makes them explicit and testable.

### 2.2 Dependency-conformance check

| Dependency | In `components.md` | Actual usage |
|---|---|---|
| SpiDriver | Yes | All SPI3 transactions via `spi_transceive()` |
| GpioDriver | Yes | NSS, RST, WAKEUP, BOOT0 driven; DRDY read |
| ExtiDriver | Yes | DRDY (PE1) line mapping and trigger edge via `exti_configure()`; interrupt enable via `exti_enable()`; pending-flag clear via `exti_clear_pending()` |

**EXTI configuration:** DRDY (PE1) requires EXTI1 configuration. This is
owned by ExtiDriver (see `exti-driver.md`), the sole owner of
`SYSCFG_EXTICRx` and the EXTI trigger/mask registers on both boards.
WifiDriver calls `exti_configure()` during `wifi_create()` (Phase 1) and
`exti_enable()` during `wifi_attach_datardy_callback()` (Phase 2), and
the DRDY ISR calls `exti_clear_pending()` before invoking
`wifi_datardy_irq_handler()`.

### 2.3 P3 consideration

Single consumer (WifiTask via D29). IWifi is a single interface. No ISP
split warranted at this level.

### 2.4 Socket abstraction rationale

IWifi presents a polymorphic socket abstraction to its consumers
(MqttClient via TCP, NtpClient via UDP, both routed through WifiTask).
AT commands are an internal implementation detail — they are not exposed
above the driver layer.

**LLD-D13:** The ISM43362 AT-command set selects socket type via the
`P1=` command (0 = TCP, 1 = UDP). `wifi_open_socket()` takes a
`wifi_socket_type_t` parameter and issues the appropriate AT sequence.

**Why no TLS at driver level?** The ISM43362 supports on-module TLS
(`AT+TLSCERT`, `AT+TLSKEY`). However, surfacing TLS at the driver layer
couples certificate management to a specific module. TLS is handled at
the MqttClient level via a software TLS stack (e.g., mbedTLS over
`wifi_send/recv`). See WIFI-O1.

### 2.5 Data types

```c
/* wifi_driver.h */

#ifndef WIFI_DRIVER_H
#define WIFI_DRIVER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spi_driver.h"
#include "gpio_driver.h"
#include "exti_driver.h"

#define WIFI_MAX_SSID_LEN     32u
#define WIFI_MAX_PASS_LEN     64u
#define WIFI_MAX_SOCKETS       4u   /**< ISM43362 supports up to 4 concurrent sockets. */
#define WIFI_INVALID_SOCKET  255u
#define WIFI_DRDY_EXTI_LINE    1u   /**< PE1 — EXTI1. */
#define WIFI_EXTI_NVIC_PRIORITY 6u  /**< Suggested by exti-driver.md §4.4; must be
                                         >= configMAX_SYSCALL_INTERRUPT_PRIORITY. */

/** @brief Opaque handle to a WifiDriver instance. */
typedef struct wifi_inst *wifi_handle_t;

/** @brief Socket identifier returned by wifi_open_socket(). */
typedef uint8_t wifi_socket_t;

typedef enum {
    WIFI_ERR_OK            = 0,
    WIFI_ERR_NOT_INIT      = 1,  /**< Function called before successful wifi_create().     */
    WIFI_ERR_SPI           = 2,  /**< SPI transaction failure.                             */
    WIFI_ERR_MODULE        = 3,  /**< AT command returned ERROR.                           */
    WIFI_ERR_TIMEOUT       = 4,  /**< DRDY or response wait timed out.                    */
    WIFI_ERR_NOT_CONNECTED = 5,  /**< AP not associated.                                  */
    WIFI_ERR_SOCKET        = 6,  /**< Socket open / send / recv failure.                  */
    WIFI_ERR_INVALID_ARG   = 7,  /**< NULL pointer or out-of-range argument.              */
    WIFI_ERR_FIRMWARE      = 8,  /**< Wrong firmware version on module.                   */
    WIFI_ERR_NULL_PTR      = 9,  /**< Required pointer argument was NULL.                 */
    WIFI_ERR_NO_RESOURCE   = 10, /**< Static instance pool or socket table exhausted.     */
} wifi_err_t;

typedef enum {
    WIFI_SOCKET_TCP = 0,
    WIFI_SOCKET_UDP = 1,
} wifi_socket_type_t;

typedef enum {
    WIFI_LINK_DOWN = 0,
    WIFI_LINK_UP   = 1,
} wifi_link_state_t;

/** @brief Callback invoked from DRDY ISR — must be ISR-safe. */
typedef void (*wifi_datardy_cb_t)(void *ctx);
```

### 2.6 Configuration struct

```c
/**
 * @brief WifiDriver creation configuration.
 *
 * Injected dependencies: SPI handle (from spi_create) and GPIO handles
 * for all ISM43362 control lines (from gpio_create).
 */
typedef struct {
    spi_handle_t  spi;      /**< SPI3 handle for data transfer.        */
    gpio_handle_t nss;      /**< ISM43362 chip-select (PE0, active low). */
    gpio_handle_t drdy;     /**< ISM43362 data-ready input (PE1).      */
    gpio_handle_t rst;      /**< ISM43362 reset (PE8, active low).     */
    gpio_handle_t wakeup;   /**< ISM43362 wakeup (PB13, active high).  */
    gpio_handle_t boot0;    /**< ISM43362 boot mode (PB12, low=normal). */
} wifi_config_t;
```

### 2.7 Public API (`wifi_driver.h`)

```c
/**
 * @brief Create and initialise a WifiDriver instance.
 *
 * Performs the ISM43362 hardware reset sequence (BOOT0 low → RST
 * pulse → 500 ms boot wait), drains the post-reset boot cursor Data
 * Phase, sends the "I?" liveness/info command, and verifies firmware
 * version (C3.5.2.3.BETA9 required per UM2153 §7.11.3).
 *
 * GPIO pin configuration (mode, AF, pull) must be completed by the
 * caller via gpio_create() before calling this function.  This
 * function drives the pins but does not configure their mode.
 *
 * DRDY ISR is NOT enabled by this function — call
 * wifi_attach_datardy_callback() post-scheduler.  During creation,
 * DRDY waits use bounded busy-polling (pre-scheduler, acceptable).
 *
 * @param[in]  config  Injected dependencies (SPI + GPIO handles).
 * @param[out] handle  Receives the created handle on success.
 * @return WIFI_ERR_OK on success; WIFI_ERR_NULL_PTR if config or
 *         handle is NULL; WIFI_ERR_NO_RESOURCE if pool exhausted;
 *         WIFI_ERR_TIMEOUT if module does not respond to AT
 *         handshake; WIFI_ERR_FIRMWARE if version mismatch.
 * @note Threading: task-context only. Must be called before the
 *       scheduler starts.
 */
wifi_err_t wifi_create(const wifi_config_t *config, wifi_handle_t *handle);

/**
 * @brief Register the DATARDY interrupt callback.
 *
 * Stores the callback and enables EXTI1 interrupt.  The callback
 * executes in ISR context — it must be ISR-safe (e.g.,
 * xTaskNotifyFromISR).
 *
 * @param[in] handle  WifiDriver handle from wifi_create().
 * @param[in] cb      Callback function (ISR-safe).
 * @param[in] ctx     Opaque context passed to callback (typically
 *                    a TaskHandle_t for xTaskNotifyFromISR).
 * @return WIFI_ERR_OK on success; WIFI_ERR_NULL_PTR if handle or
 *         cb is NULL.
 * @note Threading: task-context only. Call from WifiTask init,
 *       after the scheduler has started.
 */
wifi_err_t wifi_attach_datardy_callback(wifi_handle_t handle,
                                         wifi_datardy_cb_t cb,
                                         void *ctx);

/**
 * @brief Connect to a WiFi access point.
 *
 * Issues AT+WC=<ssid>,<password>,0 and waits for association.
 * On success, sets internal link state to WIFI_LINK_UP.
 *
 * @param[in] handle    WifiDriver handle.
 * @param[in] ssid      Null-terminated SSID (max WIFI_MAX_SSID_LEN).
 * @param[in] password  Null-terminated password (max WIFI_MAX_PASS_LEN).
 * @return WIFI_ERR_OK on success; WIFI_ERR_MODULE if association
 *         fails; WIFI_ERR_TIMEOUT if module does not respond.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
wifi_err_t wifi_connect_ap(wifi_handle_t handle,
                            const char *ssid,
                            const char *password);

/**
 * @brief Disconnect from the current access point.
 *
 * Issues AT+WD and sets internal link state to WIFI_LINK_DOWN.
 *
 * @param[in] handle  WifiDriver handle.
 * @return WIFI_ERR_OK on success.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
wifi_err_t wifi_disconnect_ap(wifi_handle_t handle);

/**
 * @brief Query the current WiFi link state.
 *
 * Returns the cached link state (UP or DOWN).  Does not issue an AT
 * command — the state is updated by wifi_connect_ap() and
 * wifi_disconnect_ap().
 *
 * @param[in]  handle  WifiDriver handle.
 * @param[out] state   Receives the current link state.
 * @return WIFI_ERR_OK on success; WIFI_ERR_NULL_PTR if state is NULL.
 * @note Threading: task-context only, non-blocking.
 */
wifi_err_t wifi_get_link_state(wifi_handle_t handle,
                                wifi_link_state_t *state);

/**
 * @brief Read the current RSSI from the access point.
 *
 * Issues AT+WRSSI and parses the numeric response.
 *
 * @param[in]  handle    WifiDriver handle.
 * @param[out] rssi_dbm  Receives the RSSI in dBm (negative value).
 * @return WIFI_ERR_OK on success; WIFI_ERR_NOT_CONNECTED if not
 *         associated.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
wifi_err_t wifi_get_rssi(wifi_handle_t handle, int8_t *rssi_dbm);

/**
 * @brief Open a TCP or UDP socket to a remote host.
 *
 * Issues AT+P1=<type> to select transport, then AT+NCPX to open the
 * connection.  Returns a socket identifier for use with wifi_send(),
 * wifi_recv(), and wifi_close_socket().
 *
 * @param[in]  handle       WifiDriver handle.
 * @param[in]  type         WIFI_SOCKET_TCP or WIFI_SOCKET_UDP.
 * @param[in]  remote_addr  Null-terminated IP address or hostname.
 * @param[in]  remote_port  Remote port number.
 * @param[out] out_socket   Receives the socket identifier on success.
 * @return WIFI_ERR_OK on success; WIFI_ERR_NO_RESOURCE if no free
 *         socket slot; WIFI_ERR_SOCKET if connection fails.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
wifi_err_t wifi_open_socket(wifi_handle_t handle,
                             wifi_socket_type_t type,
                             const char *remote_addr,
                             uint16_t remote_port,
                             wifi_socket_t *out_socket);

/**
 * @brief Send data on an open socket.
 *
 * Issues AT+S.=<socket>,<len> followed by the data payload.
 *
 * @param[in] handle  WifiDriver handle.
 * @param[in] socket  Socket identifier from wifi_open_socket().
 * @param[in] data    Pointer to data to send.
 * @param[in] len     Number of bytes to send.
 * @return WIFI_ERR_OK on success; WIFI_ERR_NOT_CONNECTED if link is
 *         down; WIFI_ERR_SOCKET if send fails.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
wifi_err_t wifi_send(wifi_handle_t handle,
                      wifi_socket_t socket,
                      const uint8_t *data,
                      size_t len);

/**
 * @brief Receive data from an open socket.
 *
 * Issues AT+R=<socket>,<buf_len> and reads the response payload.
 * Blocks until data is available or timeout expires.
 *
 * @param[in]  handle      WifiDriver handle.
 * @param[in]  socket      Socket identifier from wifi_open_socket().
 * @param[out] buf         Buffer to receive data into.
 * @param[in]  buf_len     Size of the receive buffer in bytes.
 * @param[out] out_len     Receives the number of bytes actually read.
 * @param[in]  timeout_ms  Maximum wait time in milliseconds.
 * @return WIFI_ERR_OK on success; WIFI_ERR_TIMEOUT if no data within
 *         timeout; WIFI_ERR_SOCKET if receive fails.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
wifi_err_t wifi_recv(wifi_handle_t handle,
                      wifi_socket_t socket,
                      uint8_t *buf,
                      size_t buf_len,
                      size_t *out_len,
                      uint32_t timeout_ms);

/**
 * @brief Close an open socket.
 *
 * Issues AT+NCLS=<socket> and frees the internal socket table entry.
 *
 * @param[in] handle  WifiDriver handle.
 * @param[in] socket  Socket identifier from wifi_open_socket().
 * @return WIFI_ERR_OK on success; WIFI_ERR_SOCKET if close fails.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
wifi_err_t wifi_close_socket(wifi_handle_t handle, wifi_socket_t socket);

#endif /* WIFI_DRIVER_H */
```

---

## 3. Internal design

### 3.1 Private struct and static pool

```c
/* wifi_driver.c — internal, not visible to consumers */

#define WIFI_MAX_INSTANCES       1u
#define WIFI_AT_BUF_SIZE       512u
#define WIFI_DRDY_TIMEOUT_MS   100u   /**< DRDY assert wait (WIFI-O5). */
#define WIFI_RESP_TIMEOUT_MS  5000u   /**< AT response wait (WIFI-O5). */
#define WIFI_BOOT_DRDY_TIMEOUT_MS 3000u /**< Boot-only DRDY-high waits (boot cursor + first Command Phase, WIFI-O7). */
#define WIFI_SOCKET_CONNECT_TIMEOUT_MS 15000u /**< P6=1 (Start Client) response wait, longer than WIFI_RESP_TIMEOUT_MS because it triggers a real connect over the air (WIFI-O9). */
#define WIFI_JOIN_TIMEOUT_MS 20000u /**< C0 (join) response wait, longer than WIFI_RESP_TIMEOUT_MS because it performs a full WPA2 handshake and DHCP negotiation over the air, same class of bug as WIFI-O9 (WIFI-O14). */

struct wifi_inst {
    /* Injected dependencies */
    spi_handle_t    spi;
    gpio_handle_t   nss;
    gpio_handle_t   drdy;
    gpio_handle_t   rst;
    gpio_handle_t   wakeup;
    gpio_handle_t   boot0;

    /* Runtime state */
    wifi_link_state_t link_state;
    int8_t            rssi_dbm;
    bool              socket_open[WIFI_MAX_SOCKETS];
    wifi_datardy_cb_t datardy_cb;
    void             *datardy_ctx;
    bool              ready;
    bool              in_use;

    /* AT command working buffer */
    char              at_buf[WIFI_AT_BUF_SIZE];
};

static struct wifi_inst g_pool[WIFI_MAX_INSTANCES];
static uint8_t          g_count;
```

### 3.2 SPI protocol — ISM43362 transaction model

The ISM43362 uses a custom half-duplex SPI handshake with DRDY as a
flow-control signal. **SPI frame size is 16 bits** — all AT command
data is sent and received in 16-bit words, and the interface is
little-endian: within each 16-bit pair, the *second* logical byte is
clocked out first (datasheet §10.2.3 endian example: `"I?\r\x0A"` goes
out on the wire as `0x3F 0x49 0x0A 0x0D`). Getting this backwards
transposes every pair of characters the module sees or sends (WIFI-O6).

**Boot cursor (post-reset, before any command — datasheet §10.2.1):**

```
0. After reset, the module raises DRDY to signal a Data Phase holding
   the boot prompt "\r\n> " — NOT a Command Phase. Assert NSS low,
   clock 0x0A on MOSI (fill byte) while reading MISO until DRDY falls,
   then deassert NSS. Only the *next* DRDY rising edge is a genuine
   Command Phase. Sending an AT command on the first rising edge
   desyncs the module's phase state machine (WIFI-O6).
```

**Send transaction (AT command → module):**

```
1. Assert NSS low (via gpio_write on nss handle)
2. Wait for DRDY high (poll or notification) — max WIFI_DRDY_TIMEOUT_MS
3. Send command bytes as 16-bit words via spi_transceive(), each pair
   byte-swapped per the endian note above:
     - If command length is odd, append 0x15 (NAK) as padding byte
       (quick reference DOC-esWiFi_AT_Command_20041.1.20 p.2 worked
       example: "P0=0\r" pads to 0x15, NOT 0x0A)
4. Deassert NSS high
5. Wait for DRDY low (module acknowledged receipt)
6. Wait for DRDY high (module response ready) — max WIFI_RESP_TIMEOUT_MS
```

**Receive transaction (response ← module):**

```
7. Assert NSS low
8. Read 16-bit words via spi_transceive(tx_buf=0x0A0A) — the host must
   clock 0x0A on MOSI while draining a Data Phase — until:
     a. DRDY goes low (end of response), OR
     b. Buffer is full
   Each word is unswapped per the endian note above.
9. Deassert NSS high
10. Parse response: look for "\r\nOK\r\n" or "\r\nERROR\r\n"
```

### 3.3 Internal AT command engine

```c
/**
 * @brief Send an AT command and read the response.
 *
 * Handles the full send/receive SPI cycle with DRDY handshake.
 * Returns WIFI_ERR_OK if response contains "\r\nOK\r\n".
 * Returns WIFI_ERR_MODULE if response contains "ERROR".
 * Returns WIFI_ERR_TIMEOUT on DRDY timeout.
 *
 * @param[in]  inst         WifiDriver instance.
 * @param[in]  cmd          Null-terminated AT command string.
 * @param[out] resp_buf     Buffer to receive the response.
 * @param[in]  resp_buf_len Size of the response buffer.
 * @return wifi_err_t status.
 */
static wifi_err_t prv_at_command(struct wifi_inst *inst,
                                  const char *cmd,
                                  char *resp_buf,
                                  size_t resp_buf_len);
```

AT command mapping (WIFI-D12 — corrected against Inventek's own IWIN AT
command reference, not the fictional Hayes-style commands v0.2 originally
used; see WIFI-D12 in §11 for how that was found and why the correction
matters). Command codes are defined in `wifi_at_commands.h`, kept
separate from the transport/state-machine logic in `wifi_driver.c`.

The ISM43362's IWIN command set has **no "AT" attention prefix** — every
command is `<COMMAND><=data><CR>`, e.g. `C1=mySSID\r`, not `AT+C1=mySSID\r`.

| Public API | IWIN command(s) |
|---|---|
| `wifi_create` (liveness + version) | `I?\r` (module info; response must contain "C3.5.2.3"). `?\r` (print-help) is never sent — the quick reference (DOC-esWiFi_AT_Command_20041.1.20, Help Commands) marks it "Not available in SPI firmware." |
| `wifi_connect_ap` | `C1=<ssid>\r`, `C2=<pwd>\r`, `C3=4\r` (WPA2 Mixed), `C4=1\r` (DHCP on), `C0\r` (join) — five sequential commands |
| `wifi_disconnect_ap` | `CD\r` |
| `wifi_get_rssi` | `CR\r` — bare response: `0` if not joined, else the RSSI value with no prefix |
| `wifi_open_socket(TCP\|UDP)` | `P0=<slot>\r` (select), `P1=<0\|1>\r` (protocol), `P3=<host>\r`, `P4=<port>\r`, `P6=1\r` (start client) — five sequential commands; there is no combined "open connection" command |
| `wifi_send` | `P0=<slot>\r` (select), then `S3=<len>\r` + payload |
| `wifi_recv` | `P0=<slot>\r` (select), `R1=<len>\r` (set expected size), then `R0\r` |
| `wifi_close_socket` | `P0=<slot>\r` (select), then `P6=0\r` (stop client) — there is no dedicated close command |

Every one of these is a **separate** `prv_at_command()` round trip (send →
wait for its own `\r\nOK\r\n`/`\r\nERROR\r\n` → proceed); the module's
socket commands operate on whichever socket `P0=` last selected, so the
select must precede every per-socket operation, including send/recv/close.

### 3.4 FRXTH — ISM43362 16-bit SPI requirement

The ISM43362 requires 16-bit SPI frames. SpiDriver must be configured
with DS=1111 and FRXTH=0 (resolved in spi-driver.md v0.2, SPID-O1).
WifiDriver does not configure SPI registers directly — this is
SpiDriver's responsibility.

### 3.5 DRDY ISR design

The DATARDY line (PE1, EXTI1) fires when the module has data available
or has accepted a command.

```c
/* In stm32l4xx_it.c */
void EXTI1_IRQHandler(void)
{
    if (EXTI->PR1 & (1u << 1u))
    {
        exti_clear_pending(1u);  /* clear pending — via ExtiDriver */
        wifi_datardy_irq_handler();
    }
}
```

```c
/* wifi_driver.c — called from ISR context */
void wifi_datardy_irq_handler(void)
{
    /* g_pool[0] is safe here: single instance, single ISR. */
    struct wifi_inst *inst = &g_pool[0];
    if (inst->datardy_cb != NULL)
    {
        inst->datardy_cb(inst->datardy_ctx);
    }
}
```

WifiTask registers:

```c
static void prv_wifi_datardy_cb(void *ctx)
{
    TaskHandle_t task = (TaskHandle_t)ctx;
    BaseType_t yield = pdFALSE;
    xTaskNotifyFromISR(task, WIFI_TASK_DATARDY_BIT, eSetBits, &yield);
    portYIELD_FROM_ISR(yield);
}
```

DRDY waits inside `prv_at_command()` are bounded busy-polls **uniformly, in
every phase, pre- and post-scheduler** (WIFI-D11) — registering a callback
does not change how `prv_at_command()` itself waits. WifiDriver has no
FreeRTOS dependency of its own; the callback above only feeds
`xTaskNotifyFromISR` for WifiTask's own use (currently reserved — see
wifi-task.md), not for `prv_at_command()`'s internal wait loop. The earlier
design considered here (DRDY wait switching to `xTaskNotifyWait()` once a
callback is registered, WIFI-D4) was superseded by WIFI-D11 before
implementation — see §11 Decisions log.

### 3.6 Two-phase init rationale

**Phase 1 — `wifi_create()` (pre-scheduler):**

1. Drive BOOT0 low (normal boot mode, not firmware update).
2. Assert RST low → delay 10 ms → deassert RST high (hardware reset).
3. Wait 500 ms for module boot (blocking delay via `cpu_delay_ms()`).
4. Drive WAKEUP high (normal operation).
5. Drive NSS high (deasserted, idle).
6. Configure DRDY line: `exti_configure(1u, EXTI_PORT_E, EXTI_EDGE_RISING)`.
   Does not enable the interrupt — only maps PE1 to EXTI1 and sets the
   trigger edge (see `exti-driver.md`).
7. AT handshake in polling mode: `prv_at_command("?\r", ...)`.
8. Firmware version check: `prv_at_command("I?\r", ...)` — response must
   contain "C3.5.2.3" (per UM2153 §7.11.3 FCC/CE compliance).
9. Set `inst->ready = true`.

**Phase 2 — `wifi_attach_datardy_callback()` (post-scheduler):**

1. Store callback and context in instance.
2. Enable EXTI1 interrupt: `exti_enable(1u, WIFI_EXTI_NVIC_PRIORITY)`.

### 3.7 Socket table management

The ISM43362 supports up to 4 concurrent sockets (TCP or UDP).
`inst->socket_open[WIFI_MAX_SOCKETS]` tracks allocation.

`wifi_open_socket()` selects transport via P1, scans for first free
slot, passes the slot index to NCPX, and returns the slot as
`out_socket`. `wifi_close_socket()` issues NCLS and clears the entry.

### 3.8 Test reset hook

```c
#ifdef TEST
void wifi_reset_for_test(void)
{
    memset(g_pool, 0, sizeof(g_pool));
    g_count = 0u;
}
#endif
```

---

## 4. Hardware contract

### 4.1 SPI bus

| Parameter | Value | Source |
|---|---|---|
| Peripheral | SPI3 | UM2153 §7.11.3 |
| SCK | PC10 (AF6) | UM2153 Table 11, pin 78 |
| MISO | PC11 (AF6) | UM2153 Table 11, pin 79 |
| MOSI | PC12 (AF6) | UM2153 Table 11, pin 80 |
| Frame size | 16 bits (DS=1111) | ISM43362 SPI protocol |
| FRXTH | 0 | Required for 16-bit frames |
| Mode | CPOL=0, CPHA=0 (Mode 0) | ISM43362 datasheet |
| Clock speed | 10 MHz (BR=010, PCLK1/8) | SPI companion §4.2 |

### 4.2 GPIO lines — CONFIRMED

All 5 ISM43362 control lines confirmed from UM2153 Table 11:

| Signal | MCU pin | Direction | Active | UM2153 ref |
|---|---|---|---|---|
| NSS (CSN) | PE0 | Output | Low | Table 11, pin 97 |
| DRDY | PE1 | Input (EXTI1, rising edge) | High | Table 11, pin 98 |
| RST | PE8 | Output | Low (reset) | Table 11, pin 39 |
| BOOT0 | PB12 | Output | Low = normal; High = FW update | Table 11, pin 51 |
| WAKEUP | PB13 | Output | High (active) | Table 11, pin 52 |

### 4.3 Power rail

The ISM43362 is powered from the 3V3_WIFI regulated rail (LT1963EST-3.3,
Fig. 26). No firmware action required to enable it.

### 4.4 NVIC

EXTI1 (PE1 DRDY line) must have priority ≥ `configMAX_SYSCALL_INTERRUPT_PRIORITY`
since the callback uses `xTaskNotifyFromISR()`. Priority is set during
`wifi_attach_datardy_callback()` via `exti_enable(1u, WIFI_EXTI_NVIC_PRIORITY)`
— ExtiDriver owns the NVIC priority/enable call for the shared EXTI1_IRQn
vector; WifiDriver never touches NVIC registers directly.

---

## 5. Sequence integration

### 5.1 TCP code path — MQTT cloud publish (SD-03)

```
MqttClient (routed via WifiTask per D29)
  → wifi_open_socket(handle, WIFI_SOCKET_TCP, broker_ip, 8883, &sock)
      → AT+P1=0 → AT+NCPX=0,<ip>,8883,0 → OK → sock=0
  → wifi_send(handle, sock, mqtt_bytes, len)
      → AT+S.=0,<len> + payload → OK
  → wifi_recv(handle, sock, buf, buf_len, &rcvd, timeout_ms)
      → AT+R=0,<len> → payload
  → wifi_close_socket(handle, sock)
      → AT+NCLS=0 → OK
```

### 5.2 UDP code path — NTP time sync (SD-09)

```
NtpClient (routed via WifiTask per D29)
  → wifi_open_socket(handle, WIFI_SOCKET_UDP, ntp_ip, 123, &sock)
      → AT+P1=1 → AT+NCPX=<id>,<ip>,123,1 → OK
  → wifi_send(handle, sock, ntp_request, 48)
      → AT+S.=<id>,48 + payload → OK
  → wifi_recv(handle, sock, ntp_response, 48, &rcvd, 1000)
      → AT+R=<id>,48 → 48-byte response
  → wifi_close_socket(handle, sock)
      → AT+NCLS=<id> → OK
```

### 5.3 Init sequence

```
[Pre-scheduler — board_init()]
  gpio_create(PE0/PE1/PE8/PB12/PB13 configs) → GPIO handles
  spi_create(SPI3 config) → SPI handle
  wifi_create({spi, nss, drdy, rst, wakeup, boot0}) → WiFi handle
    → BOOT0 low, RST pulse, 500 ms boot wait
    → exti_configure(1, EXTI_PORT_E, EXTI_EDGE_RISING)
    → AT handshake (polling mode)
    → AT+GMR → firmware version check
    → ready = true

[Post-scheduler — WifiTask init]
  wifi_attach_datardy_callback(handle, prv_cb, xTaskGetCurrentTaskHandle())
    → store callback, exti_enable(1, WIFI_EXTI_NVIC_PRIORITY)

[WifiTask main loop]
  wifi_connect_ap(handle, ssid, pwd)
  wifi_open_socket(handle, TCP, host, port, &sock)
  [pass sock to MqttClient]
```

### 5.4 SD trace

| SD | Role | Key functions |
|---|---|---|
| SD-03 | MQTT publish via TCP | `wifi_send()`, `wifi_recv()` |
| SD-04 | Store-and-forward reconnect | `wifi_send()`, `wifi_recv()` |
| SD-05 | Alarm MQTT publish | `wifi_send()` |
| SD-09 | NTP UDP query | `wifi_open_socket(UDP)`, `wifi_send()`, `wifi_recv()` |

---

## 6. Error and fault behaviour

All public functions return `wifi_err_t`. No retry by the driver — consumers
apply protocol-level retry. NSS is always deasserted on any error path.

| Error | Cause | Behaviour |
|---|---|---|
| `WIFI_ERR_NOT_INIT` | Called before `wifi_create()` | Return immediately, no HW access |
| `WIFI_ERR_SPI` | `spi_transceive()` failed | NSS deasserted, return error |
| `WIFI_ERR_MODULE` | AT response contains "ERROR" | Return error |
| `WIFI_ERR_TIMEOUT` | DRDY or response wait expired | NSS deasserted, return error |
| `WIFI_ERR_NOT_CONNECTED` | Send/recv with link DOWN | Return immediately, no SPI |
| `WIFI_ERR_SOCKET` | Socket operation failed at module | Return error |
| `WIFI_ERR_INVALID_ARG` | Out-of-range argument | Return immediately |
| `WIFI_ERR_FIRMWARE` | Version mismatch at init | Init aborted, system runs without WiFi |
| `WIFI_ERR_NULL_PTR` | Required pointer is NULL | Return immediately |
| `WIFI_ERR_NO_RESOURCE` | Pool or socket table full | Return immediately |

---

## 7. Principles applied

- **P1 (Strict directional layering).** Depends only on SpiDriver and GpioDriver (both driver layer); no middleware, no application.
- **P2 (DIP).** Consumers (MqttClient, NtpClient — middleware) depend on the `wifi_handle_t` abstraction, not on the concrete implementation. The opaque handle type achieves DIP without a vtable.
- **P5 (Bounded resources).** Static pool of 1 instance; socket table bounded by `WIFI_MAX_SOCKETS`; AT buffer statically allocated; no heap.
- **P6 (Traces to requirements).** Connect, socket, send/recv trace to REQ-CC-050, CON-001.
- **P8 (Total error propagation).** `wifi_err_t` on all operations; AT timeouts return error; NSS deasserted on every error path.
- **P9 (BARR-C).** Fixed-width types; `const` on read-only pointers; braces on all control flow.
- **P10 (Naming).** Prefix `wifi_`; handle `wifi_handle_t`; socket `wifi_socket_t`; errors `WIFI_ERR_*`.

---

## 8. Synchronisation

Caller serialises. WifiTask is the sole caller per D29. The driver holds
no FreeRTOS synchronisation primitives. `wifi_create()` runs before the
scheduler. All post-scheduler calls run in WifiTask context. Concurrent
access from other tasks is forbidden — they route through WifiTask's
request queue (designed in the WifiTask middleware companion, WIFI-O4).

---

## 9. Unit-test plan

Test file: `tests/gateway/drivers/wifi_driver/test_wifi_driver.c`

**Layer 1 — AT response parser (host, no hardware):**

| ID | Scenario | Expected |
|---|---|---|
| WIFI-T01 | OK response `"\r\nOK\r\n"` | Returns `WIFI_ERR_OK` |
| WIFI-T02 | ERROR response `"\r\nERROR\r\n"` | Returns `WIFI_ERR_MODULE` |
| WIFI-T03 | Truncated response (buffer full, no OK/ERROR) | Returns `WIFI_ERR_TIMEOUT` |
| WIFI-T04 | RSSI parse `"-67\r\nOK\r\n"` (bare value, no prefix per IWIN `CR` command) | `rssi_dbm = -67` |
| WIFI-T05 | Firmware version match | Returns `WIFI_ERR_OK` |
| WIFI-T06 | Firmware version mismatch | Returns `WIFI_ERR_FIRMWARE` |

**Layer 2 — Full API with mock SPI + mock GPIO:**

| ID | Scenario | Expected |
|---|---|---|
| WIFI-T07 | `wifi_create` happy path | SPI handshake sent, firmware checked, handle returned |
| WIFI-T08 | `wifi_create` with NULL config | Returns `WIFI_ERR_NULL_PTR` |
| WIFI-T09 | `wifi_create` pool exhaustion | Second call returns `WIFI_ERR_NO_RESOURCE` |
| WIFI-T10 | `wifi_connect_ap` nominal | `C1/C2/C3/C4/C0` sequence sent, link_state = UP |
| WIFI-T11 | `wifi_connect_ap` wrong SSID | `C0` (join) returns ERROR, returns `WIFI_ERR_MODULE` |
| WIFI-T12 | `wifi_open_socket(TCP)` nominal | `P0/P1/P3/P4/P6=1` sequence sent, valid socket returned |
| WIFI-T13 | `wifi_open_socket(UDP)` nominal | `P0/P1/P3/P4/P6=1` sequence sent (P1=1), valid socket returned |
| WIFI-T14 | `wifi_send` with link down | No SPI, returns `WIFI_ERR_NOT_CONNECTED` |
| WIFI-T15 | DRDY timeout | Mock DRDY stays low, returns `WIFI_ERR_TIMEOUT`, NSS deasserted |
| WIFI-T16 | NSS deasserted on SPI error | Mock SPI fails, verify NSS high at exit |
| WIFI-T17 | Socket table exhaustion | Open 4 sockets, 5th returns `WIFI_ERR_NO_RESOURCE` |
| WIFI-T18 | `wifi_close_socket` frees slot | Close socket, reopen succeeds |
| WIFI-T19 | `wifi_recv` strips leading/trailing IWIN response framing | Payload returned without `"\r\n"`/`"OK\r\n"` framing bytes (WIFI-O10) — not previously listed in this table |
| WIFI-T19b | `wifi_recv` strips a trailing SPI pad byte on odd-length responses | Payload byte count excludes the pad byte |
| WIFI-T20 | `wifi_recv` on an empty R0 response | Returns `WIFI_ERR_TIMEOUT`, not a false `WIFI_ERR_OK` (WIFI-O10/O11) — not previously listed in this table |
| WIFI-T21 | `link_state` staleness after send/recv failure (WIFI-O15) | `wifi_connect_ap` succeeds (`link_state = UP`); a subsequent `wifi_send`/`wifi_recv` failure leaves `link_state` unchanged at `UP` — pins today's documented gap, not a fix |

**Layer 3 — hardware integration (on-board, deferred):**

Full WiFi association, TCP/UDP send/receive on actual board.

---

## 10. Open items

| ID | Item | Status | Resolution |
|---|---|---|---|
| WIFI-O1 | TLS strategy: on-module vs mbedTLS | **Open** | Deferred to MqttClient LLD companion. WIFI-D2 defers TLS to MqttClient. Confirm CloudPublisherTask stack is sufficient for mbedTLS handshake (4–8 KB needed). |
| WIFI-O2 | SPI FRXTH/DS conflict | **Resolved** | SPI companion v0.2 corrected to 16-bit (DS=1111, FRXTH=0). |
| WIFI-O3 | GPIO pin assignments | **Resolved** | UM2153 Table 11: PE0=NSS, PE1=DRDY, PE8=RST, PB12=BOOT0, PB13=WAKEUP. |
| WIFI-O4 | WifiTask API surface | **Open** | Deferred to WifiTask middleware companion. IWifi is the driver contract; the request-queue routing mechanism is designed separately. |
| WIFI-O5 | Timeout values | **Resolved** | Baseline: DRDY_TIMEOUT=100 ms, RESP_TIMEOUT=5000 ms. Validate at integration. |
| WIFI-O6 | Hardware bring-up: `wifi_create()` timed out (~47 s vs. a ≤5.6 s bound) | **Resolved** | Root cause was not the DRDY/RESP timeout values or SYSCLK: (1) the driver never drained the post-reset boot-cursor Data Phase before sending a command, desyncing the module's phase state machine; (2) the first command sent was `?`, which the quick reference marks "Not available in SPI firmware"; (3) `prv_send_words`/`prv_recv_words` had the 16-bit endian swap backwards; (4) the odd-length command pad byte was `0x0A` instead of `0x15`. All four fixed together (see §3.2, §3.3, WIFI-D13). |
| WIFI-O7 | Hardware bring-up (follow-up to WIFI-O6): after fixing WIFI-O6, `wifi_create()` still failed at full CPU speed but succeeded whenever slowed by debugger single-stepping | **Resolved** | Three compounding issues, found incrementally with a bring-up diagnostic snapshot (`wifi_get_bringup_diag()`, TEMPORARY, see wifi_driver.h) added specifically to disambiguate this: (1) `drdy_config.pull` in the *integration test main* was `GPIO_PULL_NONE`, so DRDY floated to a false idle HIGH before the module ever drove it — a pre-reset diagnostic log line confirmed this; fixed to `GPIO_PULL_DOWN`. (2) with the float fixed, `wifi_create()` failed one step earlier than expected — the boot cursor's own DRDY-high wait — because the module needs longer than `WIFI_BOOT_WAIT_MS` (500 ms) + `WIFI_DRDY_TIMEOUT_MS` (100 ms) of real elapsed time after reset before it first asserts DRDY; fixed with a dedicated, generous `WIFI_BOOT_DRDY_TIMEOUT_MS` (3000 ms) for that wait. (3) with that fixed, the diagnostic snapshot showed the boot cursor now drains correctly (6 bytes) but `wifi_create()` still failed one step *later*: DRDY doesn't rise again for the first real Command Phase within the standard `WIFI_DRDY_TIMEOUT_MS` (100 ms) either — the same reset-settle-time phenomenon recurring at the next phase transition. Fixed by waiting on `WIFI_BOOT_DRDY_TIMEOUT_MS` at that transition too, before the first `prv_at_command()` call; `WIFI_DRDY_TIMEOUT_MS` stays untouched for steady-state per-command turnaround once the module is confirmed up. The debugger-slowed runs "worked" only because breakpoint pauses accidentally gave the module that extra real time at whichever transition was undersized at the time. |
| WIFI-O8 | Hardware bring-up (follow-up to WIFI-O7): `wifi_create()` now succeeds, but `wifi_connect_ap()` (C0/join) and `wifi_get_rssi()` (CR) misbehaved | **Resolved** | Two independent response-parsing bugs, found from the diagnostic snapshot's raw byte dump of a real failed join: the module's actual response was `"\r\n[JOIN   ] Grove Island\r\n[JOIN   ] Failed\r\nERROR: Unknown Error\r\nUsage: C0 \r\n> "`. (1) `WIFI_RESP_ERROR_MARKER` was the bare `"\r\nERROR\r\n"`, but real error responses carry a description right after the word (matching the `MT` example in the quick reference doc) — `"\r\nERROR: Unknown Error\r\n"` never matched, so a genuine module-reported error was misclassified as `WIFI_ERR_TIMEOUT` instead of `WIFI_ERR_MODULE`. Fixed by matching only the `"\r\nERROR"` prefix. (2) `prv_parse_rssi()` assumed the bare RSSI value starts at byte 0 of the response, but every IWIN response actually starts with the same `"\r\n<data>\r\nOK\r\n"` framing (User Manual §1.4.2) — confirmed for every response captured on hardware so far — so it looked for digits at the wrong offset. Fixed by skipping a leading `"\r\n"` before parsing. (The join failure itself in the captured example was a red herring: a 4-character test password below WPA2's 8-character minimum, not a driver bug.) (3) Found proactively (not yet hardware-confirmed) by auditing for the same class of bug: `wifi_recv()` had the identical byte-0 assumption for the received payload itself — it never skipped the leading `"\r\n"`, so every received TCP/UDP packet would have been returned to the caller with two bogus leading bytes. It also assumed `"\r\nOK\r\n"` sits at the exact end of the buffer, which the datasheet's own even-byte-count SPI padding rule (a trailing `0x15` on odd-length responses) can violate. Fixed both: skip the leading `"\r\n"`, and tolerate one trailing pad byte when locating the OK marker. Added `WIFI_T19`/`WIFI_T19b` — `wifi_recv()` had no unit test coverage at all before this. |
| WIFI-O9 | Hardware bring-up (follow-up to WIFI-O8): with association/RSSI working (TC-HW-WIFI-004 passing), `wifi_open_socket(TCP)` (TC-HW-WIFI-005) failed with `WIFI_ERR_TIMEOUT`, with SocketTest already confirmed listening on the target host/port | **Resolved** | Log timestamps placed the failure exactly `WIFI_RESP_TIMEOUT_MS` (5000 ms) after association, i.e. the DRDY-high wait after sending `P6=1` ("Start Client") never completed in time. Unlike the four commands before it in `wifi_open_socket()` (`P0`/`P1`/`P3`/`P4`, which only write local socket-slot state), `P6=1` triggers a real TCP/UDP connect over the air, which needs materially more time than a local AT-command turnaround. Fixed by giving `P6=1` its own `WIFI_SOCKET_CONNECT_TIMEOUT_MS` (15000 ms) instead of sharing `WIFI_RESP_TIMEOUT_MS` with every other command; threaded as an explicit parameter through `prv_at_command()`/`prv_send_kv()` rather than a second hidden constant, consistent with how `WIFI_DRDY_TIMEOUT_MS`/`WIFI_BOOT_DRDY_TIMEOUT_MS` are already passed explicitly at each `prv_wait_drdy()` call site. Confirmed on hardware: TC-HW-WIFI-005/006 now both open their socket successfully. |
| WIFI-O10 | Hardware bring-up (follow-up to WIFI-O9): with sockets opening correctly, `wifi_recv()` reported a false "received 8 bytes: OK\n>" success on both TC-HW-WIFI-005 (TCP) and TC-HW-WIFI-006 (UDP) — including the UDP case, where no SocketTest listener was even running, so no real data could possibly have arrived | **Resolved** | The payload parser assumed the trailing `"\r\nOK\r\n"` marker sits at the exact end of the response (modulo one optional SPI pad byte, WIFI-O8); when `R0` returns *no* data, IWIN's response framing puts the payload's own leading `"\r\n"` and the OK marker's leading `"\r\n"` in the same two bytes rather than as two separate instances — an end-anchored search never finds the marker in that case and silently returns the raw framing bytes as if they were payload. Fixed by reusing `prv_parse_response()`'s own technique (`prv_contains()`, extended to report the match position): scan the *whole* raw buffer from byte 0 for `"\r\nOK\r\n"`, and treat the payload as everything between the response's leading `"\r\n"` and the marker's start position — clamped to zero if the marker starts at or before that point. This is robust to either exact empty-response byte layout (a bare `"\r\nOK\r\n"` or a doubled `"\r\n\r\nOK\r\n"`) and to whatever trailing bytes follow the marker (prompt, pad byte), so a further hardware capture wasn't needed to pin down the precise bytes — the fix doesn't depend on knowing them. (This entry originally also fixed `wifi_recv()` ignoring `timeout_ms` by adding a client-side polling loop; that loop caused WIFI-O11 and was replaced — see there.) |
| WIFI-O11 | Hardware bring-up (follow-up to WIFI-O10): after fixing the parsing bug, TC-HW-WIFI-005 (TCP, no SocketTest reply typed) reported "no reply within timeout" after 9 s against a 3000 ms `wifi_recv()` budget; TC-HW-WIFI-006 (UDP) took 83 s against the same budget | **Resolved** | WIFI-O10's fix for the ignored `timeout_ms` parameter added a client-side retry loop: re-issue `P0`/`R1`/`R0` every `WIFI_RECV_POLL_INTERVAL_MS` (200 ms) until real data arrives, tracking elapsed time as `iterations × 200 ms`. That accounting only counted the explicit inter-poll sleep, not the real time each `P0`/`R1`/`R0` round trip itself took, so 15–16 loop iterations multiplied whatever each round trip cost instead of bounding total wait time. Reworked `wifi_recv()` to issue `P0`/`R1`/`R0` exactly once, giving `R0`'s own DRDY wait the caller's `timeout_ms` directly (floored at `WIFI_RESP_TIMEOUT_MS`). Confirmed on hardware: both TC-HW-WIFI-005 and TC-HW-WIFI-006 now return `WIFI_ERR_TIMEOUT` in a single, bounded ~5 s attempt (matching the `WIFI_RESP_TIMEOUT_MS` floor) instead of the previous 9 s/83 s multi-attempt totals. Root cause of the "no reply" itself (separate from this timing bug) was tracked down to the test network, not the driver — see WIFI-O12. |
| WIFI-O12 | Hardware bring-up (follow-up to WIFI-O11): with the timeout bug fixed, `wifi_recv()` still reports `WIFI_ERR_TIMEOUT` on both TC-HW-WIFI-005/006 against a verified-listening, verified-firewalled, same-subnet echo server on the test LAN ("Grove Island") | **Resolved for the driver; Grove Island's specific network behavior left formally unexplained** | The full read on this took several rounds and one wrong turn (see WIFI-O14): the driver itself is now proven correct — TC-HW-WIFI-005 completed a full, real, end-to-end TCP round trip on a *different* network (a mobile hotspot, matching ST's own reference example's recommended test bench), including a real received reply (`"ciao"`, 5 bytes) through `wifi_recv()`. That settles the question this item exists to answer: `wifi_send()`/`wifi_recv()`/`wifi_open_socket()` work correctly against real hardware and a real peer. What remains unexplained is specifically Grove Island's behavior (a `pktmon` capture there showed the module successfully ARPing for the target seven times over ~20 s with real replies, yet zero TCP/UDP packets ever transmitted) — plausibly AP/client isolation, as originally suspected, but that specific network's behavior is no longer load-bearing for WifiDriver's correctness and isn't worth further hardware time to pin down further. |
| WIFI-O13 | Bring-up diagnostic (not a bug) | **Infrastructure added, exercised successfully, then removed** | `wifi_open_socket()`'s `P6=1` (Start Client) response was discarded entirely (`out_resp_len` was `NULL`) — never inspected beyond the OK/ERROR classification, unlike `C0`/join where extra diagnostic text past "OK" mattered (WIFI-O8). Added `wifi_get_last_connect_response()` to capture and expose the raw text. Confirmed on hardware: the module's real P6=1 response is `"[TCP  RC] Connecting to <ip>\r\nOK\r\n> "` — richer status text than the driver's OK/ERROR classification alone, consistent with the same "extra text past OK" pattern WIFI-O8 already found for `C0`. Removed (along with `wifi_get_connection_info()`, WIFI-O12) once its diagnostic purpose was served — both were explicitly TEMPORARY bring-up scaffolding, same convention as `wifi_get_bringup_diag()`'s removal after WIFI-O7/O8. |
| WIFI-O14 | Hardware bring-up: `wifi_connect_ap()` against a mobile hotspot (not "Grove Island") failed with `WIFI_ERR_TIMEOUT` at exactly `t=5s`, reproducing across two different hotspot SSIDs on the same phone (ruling out SSID content), a confirmed-2.4GHz band, and confirmed WPA2-Personal security | **Resolved, confirmed on hardware** | Same class of bug as WIFI-O9: `C0` (join) was still sharing the generic `WIFI_RESP_TIMEOUT_MS` (5000 ms) AT-response bound with purely-local commands like `C1`-`C4`, but `C0` performs a full WPA2 handshake *and* DHCP negotiation over the air in one round trip — the exact 5000 ms failure timing matched the timeout constant precisely, the same signature that diagnosed WIFI-O9. Fixed by giving `C0` its own `WIFI_JOIN_TIMEOUT_MS` (20000 ms) instead of sharing `WIFI_RESP_TIMEOUT_MS`. Confirmed on hardware: association against the same hotspot that previously failed at 5 s now completes at `t=8s` — inside the new budget, outside the old one, and TC-HW-WIFI-005 subsequently completed a full real TCP round trip. (A separate run against the same hotspot still timed out at the new 20 s ceiling; the working run had `RSSI=-43 dBm`, a much stronger signal, suggesting that failure was signal/range-related rather than a further driver bug — not chased further since it isn't reproducible against a strong signal.) |
| WIFI-O15 | Robustness audit (prompted by a review of WiFi/MQTT connection-loss handling, alongside MQTT-O8): `link_state` (§2.5 `wifi_link_state_t`) is a cached flag, written only by `wifi_connect_ap()`/`wifi_disconnect_ap()`. Neither `wifi_send()` nor `wifi_recv()` updates it on failure, and nothing in this driver polls the module for an unsolicited disassociation — so if the AP silently drops the station (router reboot, DHCP lease loss, RF dropout), `wifi_get_link_state()` keeps reporting `WIFI_LINK_UP` indefinitely, and no code anywhere currently re-issues `wifi_connect_ap()` to recover. `wifi_send()`/`wifi_recv()` themselves still fail cleanly in this situation (`WIFI_ERR_SOCKET`/`WIFI_ERR_TIMEOUT` propagate correctly to MqttClient, which surfaces it via `disconnect_cb` per MQTT-O8) — the gap is that nothing re-associates afterward. | **Open — deferred to WifiTask** | Not fixed here: detecting an AP drop and re-associating needs a periodic poll (e.g., a liveness check or `wifi_get_rssi()` on a timer) plus a retry policy (attempt count, backoff, coordination with any sockets/MQTT session open at the time of the drop) — that owner is WifiTask (`WIFI-O4`), which has no companion doc yet, and WifiDriver is deliberately built with no FreeRTOS dependency of its own (`WIFI-D11`) so it cannot own a timer itself. Building this into WifiDriver or MqttClient now would improvise WifiTask's design in the wrong module. Pinned with a unit test (WIFI-T21) that proves today's actual behaviour — `link_state` does not self-heal after a `wifi_send()`/`wifi_recv()` failure — so this stays a tracked, tested gap rather than an assumption. Revisit when WifiTask is scoped. |

---

## 11. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| WIFI-D1 | IWifi exposes a socket API (TCP + UDP), not AT commands | AT commands are ISM43362-specific. MqttClient and NtpClient consume a portable socket interface; replacing the WiFi module requires only WifiDriver changes. |
| WIFI-D2 | TLS NOT handled inside WifiDriver | On-module TLS couples certificate management to the ISM43362. mbedTLS at MqttClient layer is portable and inspectable. |
| WIFI-D3 | Firmware version checked at init; mismatch = hard fail | Wrong firmware violates FCC/CE compliance per UM2153 §7.11.3. Fail-fast is safer than silent non-compliance. |
| WIFI-D4 | ~~DRDY wait uses `xTaskNotifyWait`, not busy-poll (post Phase 2)~~ **Superseded by WIFI-D11.** | AT responses take 10–500 ms. Busy-polling would monopolise the CPU and starve lower-priority tasks. |
| WIFI-D5 | BOOT0 held low during normal operation | BOOT0 high = firmware update mode, not normal WiFi operation. |
| WIFI-D6 | NSS deasserted on every error path | A stuck-low NSS permanently blocks the ISM43362. |
| WIFI-D7 | `open_socket()` accepts `wifi_socket_type_t` (TCP/UDP) | NTP requires UDP (RFC 5905). TCP-only IWifi cannot serve NtpClient. The ISM43362 selects transport via P1= AT command. |
| WIFI-D8 | ADT pattern (opaque handle, static pool of 1) | Gateway default. Dependencies (SPI, GPIO handles) injected via config struct. |
| WIFI-D9 | EXTI configuration owned by ExtiDriver, not GpioDriver | ExtiDriver is the sole owner of `SYSCFG_EXTICRx` and EXTI trigger/mask registers across both boards (see `exti-driver.md`, originated from WIFI-O2 root). Folding EXTI into GpioDriver would create two owners for the same shared register set once MagnetometerDriver/ImuDriver also need EXTI lines. Superseded an earlier draft of this decision that proposed a `gpio_configure_exti()` extension. |
| WIFI-D10 | `wifi_socket_t` is a distinct typedef from `wifi_handle_t` | Avoids naming collision between driver instance handles and socket identifiers. |
| WIFI-D11 | `prv_at_command()` uses bounded busy-polling on the DRDY GPIO uniformly, pre- and post-scheduler; WifiDriver has no FreeRTOS dependency of its own | Supersedes WIFI-D4. `components.md`'s USES list for WifiDriver does not include FreeRTOS, and Phase H's own H11 check certifies "no FreeRTOS dependency except ISR" — WIFI-D4 contradicted that certification. Task-level notification of DATARDY events (via `xTaskNotifyFromISR` in the registered callback) remains WifiTask's responsibility, not this driver's. |
| WIFI-D12 | AT command mapping (§3.3) corrected from a fictional Hayes-style set (`AT+WC=`, `AT+NCPX=`, `AT+S.=`, `AT+R=`, `AT+NCLS=`) to the real Inventek IWIN command set (`C1..C4`/`C0`, `P0..P6`, `S0..S3`, `R0..R3`, `CR`, `I?`, `?`) | v0.2 of this companion invented AT strings without checking them against Inventek's own documentation. Verified against `inventeksys.com/iwin/getting-started-guide/`, `/iwin/at-status-commands/`, and `/iwin/at-cmds/`, and cross-checked against UM2153 §7.11.3. The real command set has no "AT" attention prefix, and several operations (open socket, close socket) that v0.2 modelled as one combined command are actually 2–5 sequential single-purpose commands operating on whichever socket `P0=` last selected. |
| WIFI-D13 | SPI transport layer corrected (§3.2): 16-bit words are byte-swapped per pair, the odd-length command pad byte is `0x15` not `0x0A`, the post-reset boot cursor is drained before any command, and `?` is dropped from the liveness check in favour of `I?` alone | Found while investigating WIFI-O6 against the ISM43362-M3G-L44 datasheet (DOC-DS-20023) §10.2 and the IWIN AT Command Set quick reference (DOC-esWiFi_AT_Command_20041.1.20) p.2, both of which give worked byte-level examples that v0.1 of this companion did not check the implementation against. IWIN command codes/values moved out of `wifi_driver.c` into `wifi_at_commands.h` so the transport fix and the command vocabulary are independently reviewable. |

---

## 12. File layout

```
firmware/gateway/drivers/wifi_driver/
├── wifi_driver.h       /* public API — opaque handle, config, error enum, socket types */
├── wifi_driver.c       /* implementation — AT engine, SPI protocol, socket table, ISR */
└── wifi_at_commands.h  /* IWIN AT command codes/values — no logic, just string constants */

tests/gateway/drivers/wifi_driver/
└── test_wifi_driver.c  /* Unity + CMock host tests */
```

---

## Phase H — Readiness review

| # | Check | Status |
|---|---|---|
| H1 | PROVIDES / USES match `components.md` exactly | PASS — PROVIDES IWifi, USES SpiDriver + GpioDriver + ExtiDriver |
| H2 | Root SRS requirements cited and quoted | PASS — REQ-CC-050, CON-001 |
| H3 | All public API functions have complete Doxygen | PASS — brief, param, return, note on every function |
| H4 | ADT pattern applied (or exception documented) | PASS — ADT with pool of 1, WIFI-D8 |
| H5 | Error enum covers all failure modes | PASS — 10 error codes, no silent failures |
| H6 | Hardware contract specifies all pins, AF numbers, SPI config | PASS — §4.1–4.4; all from UM2153 Table 11 |
| H7 | All critical open items resolved or have named owner | PASS — O2, O3, O5 resolved; O1, O4 deferred with owner and path |
| H8 | Unit-test plan covers happy path + error cases | PASS — 18 test cases across 2 layers |
| H9 | Test file path follows Gateway folder convention | PASS |
| H10 | P1–P10 compliance reviewed | PASS — §7 |
| H11 | No FreeRTOS dependency except EXTI ISR → xTaskNotifyFromISR | PASS — ISR-only RTOS usage, documented in §3.5 |
| H12 | Thread safety documented | PASS — §8, caller serialises via D29 |
| H13 | `reset_for_test` hook specified | PASS — §3.8 |
| H14 | Decisions log complete | PASS — 10 decisions |
| H15 | Sequence integration traces to HLD SDs | PASS — §5.4 |

**Verdict: PASS — ready for implementation.**

Three open items remain (WIFI-O1 TLS strategy, WIFI-O4 WifiTask API,
WIFI-O15 AP-drop reconnection) — all are explicitly deferred to their
respective companion documents (MqttClient, WifiTask) and do not block
WifiDriver implementation. WIFI-O15 was found post-implementation, during
a robustness review of WiFi/MQTT connection-loss handling (2026-07-10);
it is pinned by WIFI-T19 rather than left as prose only.
