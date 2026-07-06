/**
 * @file wifi_driver.h
 * @brief WiFi driver — ISM43362-M3G-L44 AT-command socket interface (Gateway).
 *
 * Provides IWifi (per components.md): a portable TCP/UDP socket API over
 * the Inventek ISM43362-M3G-L44 module, reached via SPI3 (SpiDriver),
 * five GPIO control lines (GpioDriver), and the DATARDY interrupt
 * (ExtiDriver). AT commands are an internal implementation detail — never
 * exposed above this driver (WIFI-D1).
 *
 * GpioDriver is a platform singleton (free functions taking port + pin,
 * no handle type) rather than the ADT pattern this companion's config
 * originally sketched with `gpio_handle_t`. wifi_config_t therefore injects
 * a (port, pin) pair per control line instead of a handle.
 *
 * @note See docs/lld/drivers/wifi-driver.md for the full design specification.
 */

#ifndef WIFI_DRIVER_H
#define WIFI_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "exti/exti_driver.h"
#include "gpio/gpio_driver.h"
#include "spi/spi.h"

#define WIFI_MAX_SSID_LEN 32u
#define WIFI_MAX_PASS_LEN 64u
#define WIFI_MAX_SOCKETS 4u /**< ISM43362 supports up to 4 concurrent sockets. */
#define WIFI_INVALID_SOCKET 255u
#define WIFI_DRDY_EXTI_LINE 1u /**< PE1 — EXTI1. */
#define WIFI_EXTI_NVIC_PRIORITY                                                                    \
    6u /**< Suggested by exti-driver.md §4.4; must be                                             \
            >= configMAX_SYSCALL_INTERRUPT_PRIORITY. */

/** @brief Opaque handle to a WifiDriver instance. */
typedef struct wifi_inst *wifi_handle_t;

/** @brief Socket identifier returned by wifi_open_socket(). */
typedef uint8_t wifi_socket_t;

typedef enum
{
    WIFI_ERR_OK = 0,            /**< Success. */
    WIFI_ERR_NOT_INIT = 1,      /**< Function called before successful wifi_create(). */
    WIFI_ERR_SPI = 2,           /**< SPI transaction failure. */
    WIFI_ERR_MODULE = 3,        /**< AT command returned ERROR. */
    WIFI_ERR_TIMEOUT = 4,       /**< DRDY or response wait timed out. */
    WIFI_ERR_NOT_CONNECTED = 5, /**< AP not associated. */
    WIFI_ERR_SOCKET = 6,        /**< Socket open / send / recv failure. */
    WIFI_ERR_INVALID_ARG = 7,   /**< NULL pointer or out-of-range argument. */
    WIFI_ERR_FIRMWARE = 8,      /**< Wrong firmware version on module. */
    WIFI_ERR_NULL_PTR = 9,      /**< Required pointer argument was NULL. */
    WIFI_ERR_NO_RESOURCE = 10,  /**< Static instance pool or socket table exhausted. */
} wifi_err_t;

typedef enum
{
    WIFI_SOCKET_TCP = 0,
    WIFI_SOCKET_UDP = 1,
} wifi_socket_type_t;

typedef enum
{
    WIFI_LINK_DOWN = 0,
    WIFI_LINK_UP = 1,
} wifi_link_state_t;

/** @brief Callback invoked from DRDY ISR — must be ISR-safe. */
typedef void (*wifi_datardy_cb_t)(void *ctx);

/**
 * @brief WifiDriver creation configuration.
 *
 * Injected dependencies: SPI handle (from spi_create) and, per control
 * line, the GPIO port/pin pair the caller already configured (mode, AF,
 * pull) via gpio_configure_pin() before calling wifi_create().
 */
typedef struct
{
    spi_handle_t spi; /**< SPI3 handle for data transfer. */

    gpio_port_t nss_port; /**< ISM43362 chip-select port (PE0, active low). */
    uint8_t nss_pin;      /**< ISM43362 chip-select pin. */

    gpio_port_t drdy_port; /**< ISM43362 data-ready port (PE1). */
    uint8_t drdy_pin;      /**< ISM43362 data-ready pin. */

    gpio_port_t rst_port; /**< ISM43362 reset port (PE8, active low). */
    uint8_t rst_pin;      /**< ISM43362 reset pin. */

    gpio_port_t wakeup_port; /**< ISM43362 wakeup port (PB13, active high). */
    uint8_t wakeup_pin;      /**< ISM43362 wakeup pin. */

    gpio_port_t boot0_port; /**< ISM43362 boot mode port (PB12, low=normal). */
    uint8_t boot0_pin;      /**< ISM43362 boot mode pin. */
} wifi_config_t;

/**
 * @brief Create and initialise a WifiDriver instance.
 *
 * Performs the ISM43362 hardware reset sequence (BOOT0 low → RST
 * pulse → 500 ms boot wait), sends AT handshake, and verifies
 * firmware version (C3.5.2.3.BETA9 required per UM2153 §7.11.3).
 *
 * GPIO pin configuration (mode, AF, pull) must be completed by the
 * caller via gpio_configure_pin() before calling this function. This
 * function drives the pins but does not configure their mode.
 *
 * DRDY ISR is NOT enabled by this function — call
 * wifi_attach_datardy_callback() post-scheduler. DRDY waits in this
 * driver always use bounded busy-polling (see companion §3.5 for the
 * rationale: WifiDriver has no FreeRTOS dependency of its own).
 *
 * @param[in]  config  Injected dependencies (SPI + GPIO port/pin pairs).
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
 * Stores the callback and enables EXTI1 interrupt. The callback
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
wifi_err_t wifi_attach_datardy_callback(wifi_handle_t handle, wifi_datardy_cb_t cb, void *ctx);

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
wifi_err_t wifi_connect_ap(wifi_handle_t handle, const char *ssid, const char *password);

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
 * Returns the cached link state (UP or DOWN). Does not issue an AT
 * command — the state is updated by wifi_connect_ap() and
 * wifi_disconnect_ap().
 *
 * @param[in]  handle  WifiDriver handle.
 * @param[out] state   Receives the current link state.
 * @return WIFI_ERR_OK on success; WIFI_ERR_NULL_PTR if handle or state is NULL.
 * @note Threading: task-context only, non-blocking.
 */
wifi_err_t wifi_get_link_state(wifi_handle_t handle, wifi_link_state_t *state);

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
 * connection. Returns a socket identifier for use with wifi_send(),
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
wifi_err_t wifi_open_socket(wifi_handle_t handle, wifi_socket_type_t type, const char *remote_addr,
                            uint16_t remote_port, wifi_socket_t *out_socket);

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
 *         down; WIFI_ERR_INVALID_ARG if socket is out of range or not
 *         open, or the payload does not fit the AT working buffer;
 *         WIFI_ERR_SOCKET if send fails.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
wifi_err_t wifi_send(wifi_handle_t handle, wifi_socket_t socket, const uint8_t *data, size_t len);

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
 *         timeout; WIFI_ERR_INVALID_ARG if socket is out of range or
 *         not open; WIFI_ERR_SOCKET if receive fails.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
wifi_err_t wifi_recv(wifi_handle_t handle, wifi_socket_t socket, uint8_t *buf, size_t buf_len,
                     size_t *out_len, uint32_t timeout_ms);

/**
 * @brief Close an open socket.
 *
 * Issues AT+NCLS=<socket> and frees the internal socket table entry.
 *
 * @param[in] handle  WifiDriver handle.
 * @param[in] socket  Socket identifier from wifi_open_socket().
 * @return WIFI_ERR_OK on success; WIFI_ERR_INVALID_ARG if socket is out
 *         of range or not open; WIFI_ERR_SOCKET if close fails.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
wifi_err_t wifi_close_socket(wifi_handle_t handle, wifi_socket_t socket);

/**
 * @brief DATARDY (PE1 / EXTI1) ISR entry point.
 *
 * Called from EXTI1_IRQHandler() in stm32l4xx_it.c, after
 * exti_clear_pending() has already run. Invokes the registered
 * DATARDY callback, if any.
 *
 * @note Threading: ISR context only.
 */
void wifi_datardy_irq_handler(void);

#ifdef TEST
/** @brief Reset all internal state for unit testing. */
void wifi_reset_for_test(void);

/**
 * @brief Classify a raw AT response buffer as OK / ERROR / undetermined.
 *
 * Exposed for WIFI-T01..T03. Not part of the public driver contract.
 *
 * @param[in] resp      Raw response bytes (not necessarily null-terminated).
 * @param[in] resp_len  Number of valid bytes in resp.
 * @return WIFI_ERR_OK if "\r\nOK\r\n" found; WIFI_ERR_MODULE if
 *         "\r\nERROR\r\n" found; WIFI_ERR_TIMEOUT otherwise.
 */
wifi_err_t prv_parse_response(const char *resp, size_t resp_len);

/**
 * @brief Parse an AT+WRSSI response of the form "+WRSSI:-67\r\nOK\r\n".
 *
 * Exposed for WIFI-T04. Not part of the public driver contract.
 *
 * @param[in]  resp      Raw response bytes.
 * @param[in]  resp_len  Number of valid bytes in resp.
 * @param[out] out_rssi  Receives the parsed RSSI in dBm.
 * @return WIFI_ERR_OK on success; WIFI_ERR_MODULE if "+WRSSI:" not found.
 */
wifi_err_t prv_parse_rssi(const char *resp, size_t resp_len, int8_t *out_rssi);

/**
 * @brief Check an AT+GMR response for the required firmware version.
 *
 * Exposed for WIFI-T05/T06. Not part of the public driver contract.
 *
 * @param[in] resp      Raw response bytes.
 * @param[in] resp_len  Number of valid bytes in resp.
 * @return WIFI_ERR_OK if "C3.5.2.3" found; WIFI_ERR_FIRMWARE otherwise.
 */
wifi_err_t prv_check_firmware_version(const char *resp, size_t resp_len);
#endif

#endif /* WIFI_DRIVER_H */
