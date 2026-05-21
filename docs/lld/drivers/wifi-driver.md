# LLD Companion — WifiDriver (ISM43362-M3G-L44)

**Board:** Gateway (B-L475E-IOT01A)  
**Branch:** `feature/lld-wifi-driver`  
**Status:** Draft  
**Methodology:** lld-methodology.md v1.1, steps 1–8  
**Version:** 0.1
**Date:** May 2026

**HLD anchor:** WifiDriver in `components.md` (GW driver layer)

---

## 1. Sources

WifiDriver is the most complex Gateway driver. It controls the Inventek
ISM43362-M3G-L44 embedded WiFi module via an AT-command protocol over SPI3.
It is the sole driver consumed by WifiTask, which is the sole accessor of the
WiFi peripheral per D29.

| Component | Interface | PROVIDES | Root req |
|-----------|-----------|----------|----------|
| WifiDriver | SPI3 + 4 GPIO lines | IWifi | REQ-CC-050, CON-001 |

---

### 1.1 Source references

| Source | Relevant section |
|--------|-----------------|
| `components.md` | WifiDriver responsibility sentence, USES: SpiDriver, GpioDriver |
| `SRS.md` | REQ-CC-050 (WiFi connect), CON-001 (ISM43362 module), REQ-CC-010/020/030/040 (MQTT/TLS) |
| `task-breakdown.md` §5.2, §6.1, §7 | WifiTask: priority 3, 256 words; D29 (sole SPI3 owner); `SPI_wifi_IRQHandler` → WifiTask |
| `sequence-diagrams.md` SD-03, SD-04 | Cloud publish and store-and-forward flows involving WiFi |
| UM2153 §7.11.3 | ISM43362 description, SPI3, firmware version C3.5.2.3.BETA9 |
| UM2153 Appendix B, Fig. 26 | ISM43362 GPIO connections: CSN, DRDY, RST, WAKEUP, BOOT0 |
| UM2153 Appendix B, Fig. 23 | MCU pin assignments |
| ISM43362 ES-WiFi datasheet / application note | AT command set, SPI protocol, DRDY handshake |

---

## 2. Public API

### 3.1 IWifi

IWifi presents a polymorphic socket abstraction to its consumers (MqttClient
via TCP, NtpClient via UDP, both routed through WifiTask). AT commands are an
internal implementation detail — they are not exposed above the driver layer.

**LLD-D13:** The ISM43362 AT-command set selects socket type via the `P1=`
command (0 = TCP, 1 = UDP). `open_socket()` takes a `wifi_socket_type_t`
parameter and issues the appropriate AT command sequence. The handle returned
(`out_handle`) is the ISM43362 socket ID — valid for both transports.

```c
/* wifi_driver.h */

#ifndef WIFI_DRIVER_H
#define WIFI_DRIVER_H

#include <stdint.h>
#include <stddef.h>

#define WIFI_MAX_SSID_LEN    32U
#define WIFI_MAX_PASS_LEN    64U
#define WIFI_MAX_SOCKETS      4U   /* ISM43362 supports up to 4 concurrent sockets */
#define WIFI_INVALID_SOCKET 255U

typedef enum {
    WIFI_ERR_OK             = 0,
    WIFI_ERR_NOT_INIT       = 1,
    WIFI_ERR_SPI            = 2,   /* SPI transaction failure           */
    WIFI_ERR_MODULE         = 3,   /* AT command returned ERROR         */
    WIFI_ERR_TIMEOUT        = 4,   /* DRDY or response wait timed out   */
    WIFI_ERR_NOT_CONNECTED  = 5,   /* AP not associated                 */
    WIFI_ERR_SOCKET         = 6,   /* socket open / send / recv failure */
    WIFI_ERR_INVALID_ARG    = 7,
    WIFI_ERR_FIRMWARE       = 8    /* wrong firmware version on module  */
} wifi_err_t;

typedef enum {
    WIFI_SOCKET_TCP = 0,
    WIFI_SOCKET_UDP = 1
} wifi_socket_type_t;

typedef enum {
    WIFI_LINK_DOWN = 0,
    WIFI_LINK_UP   = 1
} wifi_link_state_t;

/* Callback invoked from DRDY ISR — must be ISR-safe */
typedef void (*wifi_datardy_cb_t)(void *ctx);

/* Phase 1 — pre-scheduler */
/**
 * @brief Initialise the ISM43362 Wi-Fi module via SPI.
 * @return WIFI_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Must be called before the scheduler starts.
 */
wifi_err_t wifi_init(void);

/* Phase 2 — post-scheduler, called from WifiTask init */
/**
 * @brief Register the DATARDY interrupt callback.
 * @param[in,out] cb  (description TBD)
 * @param[in,out] ctx  (description TBD)
 * @return WIFI_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Call before the scheduler starts; callback executes in ISR context.
 */
wifi_err_t wifi_attach_datardy_callback(wifi_datardy_cb_t cb, void *ctx);

/* Network association */
/**
 * @brief Connect to a Wi-Fi access point.
 * @param[in] ssid  (description TBD)
 * @param[in] password  (description TBD)
 * @return WIFI_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
wifi_err_t wifi_connect_ap(const char *ssid, const char *password);
/**
 * @brief Disconnect from the current access point.
 * @return WIFI_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
wifi_err_t wifi_disconnect_ap(void);
/**
 * @brief Query the current Wi-Fi link state.
 * @param[in,out] state  (description TBD)
 * @return WIFI_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
wifi_err_t wifi_get_link_state(wifi_link_state_t *state);
/**
 * @brief Read the current RSSI from the access point.
 * @param[in,out] rssi_dbm  (description TBD)
 * @return WIFI_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
wifi_err_t wifi_get_rssi(int8_t *rssi_dbm);

/* Polymorphic socket operations (LLD-D13) */
wifi_err_t wifi_open_socket(wifi_socket_type_t type, const char *remote_addr,
                             uint16_t remote_port, uint8_t *out_handle);
/**
 * @brief Send data on an open socket.
 * @param[in,out] handle  (description TBD)
 * @param[in] data  (description TBD)
 * @param[in,out] len  (description TBD)
 * @return WIFI_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
wifi_err_t wifi_send(uint8_t handle, const uint8_t *data, size_t len);
wifi_err_t wifi_recv(uint8_t handle, uint8_t *buf, size_t buf_len,
                     size_t *out_len, uint32_t timeout_ms);
/**
 * @brief Close an open socket.
 * @param[in,out] handle  (description TBD)
 * @return WIFI_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
wifi_err_t wifi_close_socket(uint8_t handle);

/* ------------------------------------------------------------------ */
/* Singleton vtable interface (IWifi — LLD-D10, LLD-D13)               */
/* ------------------------------------------------------------------ */

typedef struct {
    wifi_err_t (*init)(void);
    wifi_err_t (*attach_datardy_callback)(wifi_datardy_cb_t cb, void *ctx);
    wifi_err_t (*connect_ap)(const char *ssid, const char *password);
    wifi_err_t (*disconnect_ap)(void);
    wifi_err_t (*get_link_state)(wifi_link_state_t *state);
    wifi_err_t (*get_rssi)(int8_t *rssi_dbm);
    wifi_err_t (*open_socket)(wifi_socket_type_t type, const char *remote_addr,
                              uint16_t remote_port, uint8_t *out_handle);
    wifi_err_t (*send)(uint8_t handle, const uint8_t *data, size_t len);
    wifi_err_t (*recv)(uint8_t handle, uint8_t *buf, size_t buf_len,
                       size_t *out_len, uint32_t timeout_ms);
    wifi_err_t (*close_socket)(uint8_t handle);
} iwifi_t;

/** Singleton pointer to the WifiDriver vtable (Gateway only). */
extern const iwifi_t * const wifi_driver;

#endif /* WIFI_DRIVER_H */
```

**Why socket abstraction, not raw AT commands?** MqttClient and NtpClient
need to open sockets and exchange bytes. Exposing AT commands would leak the
ISM43362 protocol into middleware (P2 layering violation). IWifi hides all
module-specific protocol behind a portable socket interface — if the module is
ever replaced, only WifiDriver changes.

**Why no TLS API at driver level?** The ISM43362 supports on-module TLS
termination via AT commands (`AT+TLSCERT`, `AT+TLSKEY`). However, surfacing
TLS at the driver layer would couple certificate management to a specific
module. Decision: TLS is handled at the MqttClient level via a software TLS
stack (e.g., mbedTLS over `wifi_driver->send/recv`). See WIFI-O1.

### 3.2 Dependency-conformance check

| Dependency | In `components.md` | Actual usage |
|------------|-------------------|--------------|
| SpiDriver | Yes | Yes — all SPI3 transactions via `spi_transceive()` |
| GpioDriver | Yes | Yes — NSS, RST, WAKEUP, BOOT0 driven; DRDY read |
| ExtiDriver | No (add) | Yes — `exti_configure()` in Phase 1, `exti_enable()` in Phase 2 |

P3 (ISP): IWifi is a single interface consumed by WifiTask only (D29).
No further split is warranted at this abstraction level.

---

## 3. Internal design

### 4.1 Module structure

```
wifi_driver.h        — public API (IWifi)
wifi_driver.c        — singleton state, AT command engine, SPI protocol,
                       socket table, DRDY ISR handler
```

Private state:

```c
typedef struct {
    wifi_link_state_t   link_state;
    int8_t              rssi_dbm;
    bool                socket_open[WIFI_MAX_SOCKETS];
    wifi_datardy_cb_t   datardy_cb;
    void               *datardy_ctx;
    bool                ready;            /* set true after successful init */
} wifi_state_t;

static wifi_state_t s_wifi;
```

### 4.2 SPI protocol — ISM43362 transaction model

The ISM43362 uses a custom half-duplex SPI handshake with DRDY as a
flow-control signal. **SPI frame size is 16 bits** (two bytes per
transaction). All AT command data is sent and received in 16-bit words.

**Send transaction (AT command → module):**

```
1. Assert NSS low (GpioDriver)
2. Wait for DRDY high (poll or notification) — max WIFI_DRDY_TIMEOUT_MS
3. Send command bytes in 16-bit chunks:
     - If command length is odd, append 0x0A (LF) as padding byte
4. Deassert NSS high
5. Wait for DRDY low (module acknowledged receipt)
6. Wait for DRDY high (module response ready) — max WIFI_RESP_TIMEOUT_MS
```

**Receive transaction (response ← module):**

```
7. Assert NSS low
8. Read 16-bit chunks until:
     a. DRDY goes low (end of response), OR
     b. Buffer is full
9. Deassert NSS high
10. Parse response: look for "\r\nOK\r\n" or "\r\nERROR\r\n"
```

**Why 16-bit SPI frames?** The ISM43362 SPI protocol specification requires
16-bit data frames. This is a hardware constraint of the module. See
WIFI-O2: SpiDriver must be configured for DS=1111 (16-bit), FRXTH=0 — this
conflicts with the FRXTH=1 setting recorded in `spi-driver.md`, which assumed
8-bit frames.

### 4.3 Internal AT command engine

A private helper handles the full send-receive cycle:

```c
/*
 * prv_at_command() — internal, not in public header.
 * Sends an AT command string and reads the response into resp_buf.
 * Returns WIFI_ERR_OK if response contains "\r\nOK\r\n".
 * Returns WIFI_ERR_MODULE if response contains "ERROR".
 * Returns WIFI_ERR_TIMEOUT on DRDY timeout.
 */
static wifi_err_t prv_at_command(const char *cmd,
                                  char *resp_buf,
                                  size_t resp_buf_len);
```

All public API functions call `prv_at_command()` internally. Examples:

| Public API call | AT command sent |
|-----------------|----------------|
| `wifi_connect_ap(ssid, pwd)` | `AT+WC=<ssid>,<pwd>,0\r` |
| `wifi_disconnect_ap()` | `AT+WD\r` |
| `wifi_get_rssi()` | `AT+WRSSI\r` |
| `wifi_open_socket(TCP, host, port, &h)` | `AT+P1=0\r` (TCP) + `AT+NCPX=0,<host>,<port>,0\r` |
| `wifi_open_socket(UDP, ip, port, &h)` | `AT+P1=1\r` (UDP) + `AT+NCPX=<id>,<ip>,<port>,1\r` |
| `wifi_send(h, data, len)` | `AT+S.=<h>,<len>\r` + data payload |
| `wifi_recv(h, buf, buf_len, ...)` | `AT+R=<h>,<len>\r` |
| `wifi_close_socket(h)` | `AT+NCLS=<h>\r` |

**Firmware version check in `wifi_init()`:**

```c
prv_at_command("AT+GMR\r", resp_buf, sizeof(resp_buf));
/* Check resp_buf contains "C3.5.2.3" — if not, return WIFI_ERR_FIRMWARE */
```

Per UM2153 §7.11.3, firmware must be C3.5.2.3.BETA9 for FCC/CE compliance.
Initialisation fails immediately if the wrong firmware is detected, rather
than silently operating out of compliance.

### 4.4 DRDY ISR design

The DATARDY line (EXTI1) fires when the module has data available or has
accepted a command. The ISR calls the registered callback:

```c
/* In stm32l4xx_it.c */
void EXTI1_IRQHandler(void) {
    if (EXTI->PR1 & (1U << 1U)) {
        exti_clear_pending(1U);    /* clear pending */
        wifi_datardy_irq_handler();
    }
}
```

```c
/* wifi_driver.c — internal */
void wifi_datardy_irq_handler(void) {
    if (s_wifi.datardy_cb != NULL) {
        s_wifi.datardy_cb(s_wifi.datardy_ctx);
    }
}
```

WifiTask registers:

```c
static void prv_wifi_datardy_cb(void *ctx) {
    TaskHandle_t task = (TaskHandle_t)ctx;
    BaseType_t yield = pdFALSE;
    xTaskNotifyFromISR(task, WIFI_TASK_DATARDY_BIT, eSetBits, &yield);
    portYIELD_FROM_ISR(yield);
}
```

Inside `prv_at_command()`, DRDY wait steps use `xTaskNotifyWait()` on
`WIFI_TASK_DATARDY_BIT` with a timeout, rather than busy-polling.
This is correct because `prv_at_command()` always executes inside WifiTask
context (D29 — no other task calls WifiDriver directly).

**Why polling is wrong here:** DRDY response latency can be 10–500 ms
depending on the AT command. Busy-polling for 500 ms in a task blocks the
entire FreeRTOS CPU budget and starves lower-priority tasks. Notification
wait yields the CPU until DRDY fires.

### 4.5 Two-phase init rationale

Same as Group B sensors: DRDY callback stores a task handle that only
exists post-scheduler.

**Phase 1 — `wifi_init()` (pre-scheduler):**
1. Configure BOOT0 GPIO → low (normal boot mode, not firmware update mode).
2. Assert RST low → delay 10 ms → deassert RST high (hardware reset).
3. Wait 500 ms for module boot (blocking delay — pre-scheduler, acceptable).
4. Configure WAKEUP GPIO → high (normal operation, not power-save).
5. Configure NSS GPIO → high (deasserted, idle).
6. Configure DRDY GPIO → input via GpioDriver (no pull). Call `exti_configure(1, EXTI_PORT_X, EXTI_EDGE_RISING)` via ExtiDriver (port X to be confirmed per WIFI-O3).
7. Call `prv_at_command("AT\r", ...)` in polling mode (no DRDY ISR yet)
   to verify module is alive.
8. Call `prv_at_command("AT+GMR\r", ...)` — check firmware version.
9. Set `s_wifi.ready = true`.

**Phase 2 — `wifi_attach_datardy_callback()` (post-scheduler, WifiTask):**
1. Store callback and context.
2. Enable EXTI1 interrupt (EXTI_IMR, NVIC).

Polling mode for Phase 1: before the callback is registered, DRDY wait
steps use a short busy-poll loop (≤ 500 ms per step, bounded). This is
acceptable only during the boot sequence before the scheduler starts.

### 4.6 Socket table management

The ISM43362 supports up to 4 concurrent sockets (TCP or UDP). WifiDriver
tracks open sockets in `s_wifi.socket_open[WIFI_MAX_SOCKETS]`. The same table
covers both transports — the ISM43362 addresses all sockets by numeric ID.

`wifi_open_socket()` selects the transport type via the P1 AT command, scans
the table for the first free slot, passes the slot index to NCPX, and returns
the `out_handle` to the caller. `wifi_close_socket()` clears the entry.

---


### Synchronisation

Caller serialises. The driver holds no FreeRTOS synchronisation primitives. All entry points are intended to be called from a single task context or from `main()` before the scheduler starts. Concurrent access from multiple tasks is not safe unless the caller provides a mutex.

### wifi_init

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### wifi_attach_datardy_callback

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### wifi_connect_ap

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### wifi_disconnect_ap

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### wifi_get_link_state

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### wifi_get_rssi

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### wifi_send

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### wifi_close_socket

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### Principles applied

- **P1 (Strict directional layering).** Depends only on ISpi and GpioDriver (wakeup/flow-control pins); no upward dependencies.
- **P2 (Dependency Inversion).** Exposes `iwifi_t` vtable (IWifi); MqttClient and NtpClient depend on the interface rather than the concrete driver.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static socket table bounded by `WIFI_MAX_SOCKETS`; AT command buffer statically allocated; no heap.
- **P6 (Responsibility traces to requirements).** Connect, socket open/close, send/receive functions trace to REQ-NF-206 / REQ-NF-216 / REQ-NF-300-305 connectivity requirements.
- **P8 (Total error propagation, no silent failures).** `wifi_err_t` on all operations; AT command timeout returns error; module reset on unrecoverable error.
- **P9 (BARR-C coding standard).** IP addresses as `uint8_t[4]`; port numbers `uint16_t`; no floating-point.
- **P10 (Naming conventions).** Prefix `wifi_`; interface `IWifi` -> `iwifi_t`; errors `WIFI_ERR_*`.


## 4. Hardware contract

### 5.1 SPI bus

| Parameter | Value | Source |
|-----------|-------|--------|
| Peripheral | SPI3 | UM2153 §7.11.3 |
| SCK | PC10 | UM2153 Fig. 26 → INTERNAL-SPI3_SCK |
| MISO | PC11 | UM2153 Fig. 26 → INTERNAL-SPI3_MISO |
| MOSI | PC12 | UM2153 Fig. 26 → INTERNAL-SPI3_MOSI |
| Frame size | 16 bits (DS=1111 in SPI_CR2) | ISM43362 SPI protocol requirement |
| FRXTH | 0 (RXNE set when ≥ 16 bits in FIFO) | Required for 16-bit frames (WIFI-O2) |
| Mode | CPOL=0, CPHA=0 (Mode 0) | ISM43362 datasheet |
| Bit order | MSB first | ISM43362 datasheet |
| Clock speed | ≤ 20 MHz | ISM43362 max SPI clock |

### 5.2 GPIO lines

| Signal | MCU pin | Direction | Active | Source |
|--------|---------|-----------|--------|--------|
| NSS (CSN) | To verify (WIFI-O3) | Output | Low | UM2153 Fig. 26: ISM43362-SPI3_CSN |
| DRDY | EXTI1 — port to verify (WIFI-O3) | Input | High | UM2153 Fig. 26: ISM43362-DRDY_EXTI1 |
| RST | To verify (WIFI-O3) | Output | Low | UM2153 Fig. 26: ISM43362-RST |
| WAKEUP | To verify (WIFI-O3) | Output | High | UM2153 Fig. 26: ISM43362-WAKEUP |
| BOOT0 | To verify (WIFI-O3) | Output | High = firmware update; Low = normal | UM2153 Fig. 26: ISM43362-BOOT0 |

All GPIO pins confirmed from schematic signal labels (Fig. 26); exact
MCU port letters must be verified against UM2153 Appendix A I/O table
(WIFI-O3). This is a critical-path item — GpioDriver calls and EXTI
configuration cannot be coded without it.

### 5.3 Power rail

The ISM43362 is powered from the 3V3_WIFI regulated rail (LT1963EST-3.3
regulator, Fig. 26). This rail powers only the WiFi module. No firmware
action is required to enable it; it is controlled by hardware.

---

### Registers

N/A — the ISM43362 Wi-Fi module communicates over SPI. All SPI peripheral register access is delegated to SpiDriver. WifiDriver constructs AT-command payloads and calls `spi_transceive()`; it does not touch `SPI3->CR1`, `SPI3->DR`, or related registers directly.

### Pins

The ISM43362 GPIO control lines (NSS, DRDY, BOOT0, RST) are accessed via GpioDriver (`gpio_write_pin`, `gpio_read_pin`). Pin assignments are in §4.2 above (GPIO lines table).

### Clocks

N/A — SPI3 clock is enabled by SpiDriver (`RCC->APB1ENR1 |= RCC_APB1ENR1_SPI3EN`). GPIO clocks for the control lines are enabled by GpioDriver. No additional clock enable is required by this companion.

### NVIC

The DRDY pin (PE1, EXTI1) signals that the ISM43362 has data to send. The ISR (`EXTI1_IRQHandler`) is registered by the caller via ExtiDriver and sets `s_wifi.datardy_cb`. NVIC priority is assigned by the caller; must be ≥ `configMAX_SYSCALL_INTERRUPT_PRIORITY` if the callback notifies a FreeRTOS task (lld.md §6.3).


## 5. Sequence integration

### TCP code path — MQTT cloud publish (SD-03 reference, LLD-D13)

```
MqttClient (inside CloudPublisherTask)
  → calls WifiTask API (IWifi facade, routed via WifiTask per D29)
      → wifi_driver->open_socket(WIFI_SOCKET_TCP, broker_ip, 8883, &h)
          → AT+P1=0\r (TCP) → AT+NCPX=0,<ip>,8883,0\r → OK → h=0
      → wifi_driver->send(h, mqtt_bytes, len)
          → AT+S.=0,<len>\r + payload → OK
      → wifi_driver->recv(h, buf, buf_len, &rcvd, timeout_ms)
          → AT+R=0,<len>\r → payload
      → wifi_driver->close_socket(h)
          → AT+NCLS=0\r → OK
```

### UDP code path — NTP time sync (SD-09 reference, LLD-D13)

```
NtpClient (inside TimeServiceTask)
  → calls WifiTask API (IWifi facade, routed via WifiTask per D29)
      → wifi_driver->open_socket(WIFI_SOCKET_UDP, ntp_server_ip, 123, &h)
          → AT+P1=1\r (UDP) → AT+NCPX=<id>,<ip>,123,1\r → OK → h=<id>
      → wifi_driver->send(h, ntp_request_48_bytes, 48)
          → AT+S.=<h>,48\r + request payload → OK
      → wifi_driver->recv(h, ntp_response_buf, 48, &rcvd, 1000)
          → AT+R=<h>,48\r → 48-byte NTP response
      → wifi_driver->close_socket(h)
          → AT+NCLS=<h>\r → OK
```

### Init sequence

```
[Pre-scheduler — board_init()]
  wifi_init()
    → BOOT0 low, RST pulse, 500 ms boot wait
    → WAKEUP high, NSS high
    → polling AT handshake: "AT\r" → check alive
    → "AT+GMR\r" → check firmware version
    → s_wifi.ready = true

[Post-scheduler — WifiTask startup]
  wifi_attach_datardy_callback(prv_wifi_datardy_cb, xTaskGetCurrentTaskHandle())
    → store callback, enable EXTI1

[WifiTask main loop — on CloudPublisher request]
  wifi_connect_ap(ssid, pwd)   /* at startup or on reconnect */
  wifi_tcp_connect(host, port, &socket_id)
  [pass socket_id to MqttClient]
```

### WifiTask API boundary (D29)

CloudPublisherTask, TimeServiceTask, and UpdateServiceTask do not call
WifiDriver directly. They post requests to WifiTask via a dedicated queue
or direct-to-task API (exact IPC defined at LLD WifiTask companion stage).
WifiTask serialises all calls to WifiDriver. This eliminates the need for
any mutex on the SPI bus and matches the ISR-to-fixed-task-handle contract
(`xTaskNotifyFromISR` requires a known task handle at compile time).

---

### SD trace

| SD | Component role | Key function |
|---|---|---|
| SD-03 | SD-03a/SD-03b: `MqttClient` calls `wifi_driver_send()` / `wifi_driver_receive()` to publish telemetry and health frames to AWS IoT Core | `wifi_driver_send()`, `wifi_driver_receive()` |
| SD-04 | SD-04a: `MqttClient` uses `WifiDriver` for store-and-forward MQTT publish (cloud reconnect). SD-04b: same for drain loop | `wifi_driver_send()`, `wifi_driver_receive()` |
| SD-05 | Alarm MQTT publish travels via `MqttClient` → `WifiDriver` to cloud | `wifi_driver_send()` |
| SD-09 | `NtpClient` calls `wifi_driver_open_socket(WIFI_SOCKET_UDP)` then `wifi_driver_send()` / `wifi_driver_receive()` for the NTP UDP query | `wifi_driver_open_socket()`, `wifi_driver_send()`, `wifi_driver_receive()` |

---

## 6. Error and fault behaviour

| Condition | Response |
|-----------|----------|
| DRDY never goes high after NSS assert (WIFI_DRDY_TIMEOUT_MS elapsed) | Return `WIFI_ERR_TIMEOUT`; deassert NSS |
| AT command response contains "ERROR" | Return `WIFI_ERR_MODULE` |
| Firmware version mismatch at init | Return `WIFI_ERR_FIRMWARE`; abort init; caller logs REQ-SA-040 |
| `wifi_connect_ap()` fails (wrong credentials, AP out of range) | Return `WIFI_ERR_MODULE`; `s_wifi.link_state` remains DOWN |
| `wifi_send()` called with `link_state == DOWN` | Return `WIFI_ERR_NOT_CONNECTED` |
| `wifi_send()` called with invalid handle | Return `WIFI_ERR_INVALID_ARG` |
| SPI transceive returns error | Return `WIFI_ERR_SPI`; deassert NSS to leave bus idle |
| NULL pointer in any output parameter | Return `WIFI_ERR_INVALID_ARG` |

NSS is always deasserted on any error path to leave the SPI bus in a
known idle state. This is critical: a stuck-low NSS would permanently
block the ISM43362 from responding to future transactions.

---

## 7. Unit-test plan

WifiDriver is the hardest driver to unit-test on the host because the AT
command engine depends on a request-response SPI cycle with DRDY timing.
The test strategy uses a two-layer approach.

**Layer 1 — AT response parser (host, no hardware):**
Test `prv_at_command()` response parsing in isolation by injecting mock
response strings directly into the parser. No SPI or DRDY involved.

| Test ID | Scenario | Input | Expected |
|---------|----------|-------|----------|
| WIFI-T01 | OK response | `"\r\nOK\r\n"` | Returns `WIFI_ERR_OK` |
| WIFI-T02 | ERROR response | `"\r\nERROR\r\n"` | Returns `WIFI_ERR_MODULE` |
| WIFI-T03 | Truncated response (buffer full) | 512-byte string, no OK/ERROR | Returns `WIFI_ERR_TIMEOUT` |
| WIFI-T04 | RSSI parse | `"+WRSSI:-67\r\nOK\r\n"` | `rssi_dbm = -67` |
| WIFI-T05 | Firmware version match | `"C3.5.2.3.BETA9\r\nOK\r\n"` | `WIFI_ERR_OK` |
| WIFI-T06 | Firmware version mismatch | `"C3.5.1.0\r\nOK\r\n"` | `WIFI_ERR_FIRMWARE` |

**Layer 2 — SPI transaction sequence (host, mock SPI + mock GPIO):**
Test the full `wifi_connect_ap()` and `wifi_open_socket()` flows with mock
SPI that returns pre-canned byte sequences and mock GPIO that simulates
DRDY toggling.

| Test ID | Scenario | Expected |
|---------|----------|----------|
| WIFI-T07 | `wifi_connect_ap()` nominal | SPI sequence: `AT+WC=...` → OK; `link_state = UP` |
| WIFI-T08 | `wifi_connect_ap()` wrong SSID | SPI returns ERROR; returns `WIFI_ERR_MODULE` |
| WIFI-T09 | `wifi_open_socket(TCP, ...)` nominal | `AT+P1=0` + `AT+NCPX=...` → OK; returns valid handle |
| WIFI-T09b | `wifi_open_socket(UDP, ...)` nominal | `AT+P1=1` + `AT+NCPX=...` → OK; returns valid handle |
| WIFI-T10 | `wifi_send()` with link down | No SPI transaction; returns `WIFI_ERR_NOT_CONNECTED` |
| WIFI-T11 | DRDY timeout | Mock DRDY never goes high; returns `WIFI_ERR_TIMEOUT`; NSS deasserted |
| WIFI-T12 | NSS deasserted on SPI error | Mock SPI fails mid-transaction; verify NSS high at exit |

**Layer 3 — hardware integration (on-board):**
Full WiFi association, TCP connect to known IP, send 64 bytes, receive
echo — performed on the actual board during integration testing. Also: UDP
socket open to NTP server, send 48-byte request, receive 48-byte response.

---

## 8. Open items

### Decisions

| ID | Decision | Rationale |
|----|----------|-----------|
| WIFI-D1 | IWifi exposes a polymorphic socket API (TCP + UDP), not AT commands | AT commands are an ISM43362-specific detail. MqttClient and NtpClient consume a portable socket interface; replacing the WiFi module requires only WifiDriver changes. |
| WIFI-D7 | `open_socket()` accepts a `wifi_socket_type_t` parameter; `send/recv/close_socket` serve both transports (LLD-D13) | NTP is UDP per RFC 5905 — a TCP-only IWifi cannot serve NtpClient. The ISM43362 selects socket type via the P1= AT command; one polymorphic open_socket() covers both. Future use cases (mDNS, syslog) also benefit from generic UDP. |
| WIFI-D2 | TLS NOT handled inside WifiDriver | On-module TLS would couple certificate management to the ISM43362. mbedTLS at the MqttClient layer is portable and inspectable. Trade-off: higher MCU CPU load; acceptable given CloudPublisherTask's 8 KB stack. |
| WIFI-D3 | Firmware version checked at init; mismatch = hard fail | Operating with wrong firmware violates FCC/CE compliance per UM2153 §7.11.3. Fail-fast is safer than silently running non-compliant. |
| WIFI-D4 | DRDY wait uses `xTaskNotifyWait`, not busy-poll (post Phase 2) | AT responses can take up to 500 ms. Busy-polling for this duration inside WifiTask would monopolise the CPU and starve lower-priority tasks during connects and sends. |
| WIFI-D5 | BOOT0 held low during normal operation | BOOT0 high causes the ISM43362 to enter firmware update mode, not normal WiFi operation. Must be driven low at Phase 1 init before RST is released. |
| WIFI-D6 | NSS deasserted on every error path | A stuck-low NSS permanently blocks the ISM43362. Ensuring NSS is high on any exit path (normal or error) is mandatory for bus recovery. |

### Open items

| ID | Item | Owner | Resolution path |
|----|------|-------|-----------------|
| WIFI-O1 | TLS strategy: on-module (AT+TLSCERT) vs mbedTLS in firmware. WIFI-D2 defers TLS to MqttClient (mbedTLS). This must be confirmed against CloudPublisherTask stack (currently 8 KB) — mbedTLS TLS handshake typically needs 4–8 KB. | Luca | Confirm at MqttClient LLD companion stage. If stack insufficient, revisit to 12 KB or evaluate on-module TLS. |
| WIFI-O2 | SpiDriver `spi-driver.md` companion specifies FRXTH=1 (8-bit FIFO threshold). ISM43362 requires 16-bit SPI frames (DS=1111, FRXTH=0). These settings conflict. | Luca | Update `spi-driver.md` to document 16-bit mode specifically for WifiDriver. SpiDriver is a singleton used exclusively by WifiDriver; 16-bit mode is correct. Commit `docs: correct SpiDriver frame size to 16-bit for ISM43362`. |
| WIFI-O3 | Exact MCU port/pin assignments for all 5 GPIO lines (NSS, DRDY, RST, WAKEUP, BOOT0) are not yet confirmed. Labels available from Fig. 26 (signal names); port letters require Appendix A I/O table cross-check. | Luca | Verify against UM2153 Appendix A before coding GpioDriver init calls and EXTI1 configuration. Critical-path blocker for implementation. |
| WIFI-O4 | WifiTask API surface (how CloudPublisherTask, TimeServiceTask, UpdateServiceTask route WiFi I/O through WifiTask per D29) is not designed in this companion — it belongs to the WifiTask LLD companion at the middleware/application layer. | Luca | Design in WifiTask companion (post-driver-layer LLD). For now, IWifi is the contract; the routing mechanism is deferred. |
| WIFI-O5 | WIFI_DRDY_TIMEOUT_MS and WIFI_RESP_TIMEOUT_MS values are not defined. These bound DRDY wait and AT response wait respectively. | Luca | Baseline values: DRDY_TIMEOUT = 100 ms, RESP_TIMEOUT = 5000 ms. Validate at integration; adjust for observed module latency on association and socket operations. |

---

*This document is the LLD companion for WifiDriver. It is authored by
Luca Agrippino and reviewed by the project mentor.*
