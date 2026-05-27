/**
 * @file rtc_driver.h
 * @brief CMSIS-level RTC driver — wall-clock time and backup-register access.
 *
 * Provides IRtc (per components.md): wall-clock read/write, backup-domain
 * validity check, and indexed backup-register accessors. Used by Logger
 * (read-only, bootstrap exception) and TimeProvider (full surface).
 *
 * The driver depends only on CMSIS. It does NOT depend on FreeRTOS or any
 * other RTOS. Callers serialise themselves; the driver is not internally
 * synchronised.
 *
 * @note Realised on the backup-domain RTC of STM32F469 (Field Device) or
 *       STM32L475 (Gateway). LSE 32.768 kHz is the clock source on both.
 * @note See docs/lld/drivers/rtc-driver.md for the full design.
 */

#ifndef RTC_DRIVER_H
#define RTC_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

/* --------------------------------------------------------------------- */
/* Board-specific configuration constants                                 */
/* --------------------------------------------------------------------- */

/** Maximum backup-register index (inclusive) on STM32F469: BKP0R..BKP19R. */
#define RTC_BACKUP_MAX_IDX_F469  (19U)

/** Maximum backup-register index (inclusive) on STM32L475: BKP0R..BKP31R. */
#define RTC_BACKUP_MAX_IDX_L475  (31U)

/* --------------------------------------------------------------------- */
/* Calendar date and time                                                 */
/* --------------------------------------------------------------------- */

/**
 * @brief Calendar date and time, all fields in binary (not BCD).
 *
 * The driver converts to/from the hardware BCD format internally; consumers
 * never handle BCD.
 *
 *   year:   2000..2099 (full year, e.g. 2026)
 *   month:  1..12
 *   day:    1..31
 *   hour:   0..23 (24-hour format; 12-hour mode disabled in hardware)
 *   minute: 0..59
 *   second: 0..59
 */
typedef struct
{
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
} rtc_datetime_t;

/* --------------------------------------------------------------------- */
/* Error codes                                                            */
/* --------------------------------------------------------------------- */

/**
 * @brief RtcDriver result codes.
 */
typedef enum
{
    RTC_OK                 = 0, /**< Operation succeeded. */
    RTC_ERR_INIT_TIMEOUT   = 1, /**< INITF flag did not assert within timeout. */
    RTC_ERR_SYNC_TIMEOUT   = 2, /**< RSF flag did not assert within timeout. */
    RTC_ERR_NULL_ARG       = 3, /**< Null pointer passed to a pointer parameter. */
    RTC_ERR_BACKUP_BOUNDS  = 4, /**< Backup-register index out of range for this board. */
    RTC_ERR_LSE_NOT_READY  = 5  /**< LSE not running, or RTCSEL locked to a non-LSE source. */
} rtc_err_t;

/* --------------------------------------------------------------------- */
/* Tick source                                                            */
/* --------------------------------------------------------------------- */

/**
 * @brief Signature of the tick source used for INITF/RSF poll timeouts.
 *
 * Returns a monotonically increasing tick count. Units are implementation-
 * defined but must be consistent with the internal RTC_MS_TO_TICKS() scale.
 */
typedef uint32_t (*rtc_tick_source_fn)(void);

/* --------------------------------------------------------------------- */
/* Public API                                                             */
/* --------------------------------------------------------------------- */

/**
 * @brief Initialise the RTC peripheral.
 *
 * Must be called once from main() before the FreeRTOS scheduler starts, so
 * that Logger can read timestamps during the Init phase (bootstrap exception
 * — components.md §1 preamble).
 *
 * Behaviour:
 *  - Enables the PWR peripheral clock and sets DBP to unlock the backup
 *    domain.
 *  - Selects LSE as RTC clock source and enables the RTC clock in RCC_BDCR.
 *  - Captures backup-domain validity from RTC_ISR.INITS (immutable thereafter).
 *  - If the backup domain was reset (INITS = 0), enters init mode, configures
 *    24-hour format, writes the prescaler (PREDIV_A = 127, PREDIV_S = 255),
 *    and sets the default epoch (2000-01-01 00:00:00).
 *  - If the backup domain is valid (INITS = 1), leaves the calendar
 *    untouched.
 *  - Idempotent: a second call returns RTC_OK with no side effects.
 *
 * @return RTC_OK on success.
 *         RTC_ERR_LSE_NOT_READY if LSE is not running, or RCC_BDCR.RTCSEL is
 *         already locked to a non-LSE source.
 *         RTC_ERR_INIT_TIMEOUT if INITF does not assert within timeout
 *         (cold-start path only).
 *         RTC_ERR_SYNC_TIMEOUT if RSF does not assert within timeout
 *         (cold-start path only).
 *
 * @note Threading: task-context only. Caller ensures single invocation before
 *       the scheduler starts. NOT internally serialised. NOT ISR-safe.
 */
rtc_err_t rtc_init(void);

/**
 * @brief Read the current wall-clock time from the RTC.
 *
 * Waits for RTC_ISR.RSF to assert before reading, to avoid returning stale
 * shadow-register values immediately after rtc_set_time().
 *
 * @param[out] dt Caller-allocated rtc_datetime_t; populated on RTC_OK. Zeroed
 *                on error.
 * @return RTC_OK, RTC_ERR_NULL_ARG, or RTC_ERR_SYNC_TIMEOUT.
 *
 * @note Threading: task-context only, non-blocking (≤ 2 ms worst case). NOT
 *       ISR-safe. NOT internally serialised — caller serialises if needed.
 * @note Pre-condition (debug builds): rtc_init() has returned OK. Asserted
 *       via assert(); undefined in release builds.
 */
rtc_err_t rtc_get_time(rtc_datetime_t *dt);

/**
 * @brief Write a new wall-clock time to the RTC.
 *
 * Called by TimeProvider after a successful NTP synchronisation (REQ-TS-020).
 * Must NOT be called by Logger (bootstrap exception — Logger is read-only).
 *
 * @param dt Pointer to the new date/time. Must not be NULL.
 * @return RTC_OK, RTC_ERR_NULL_ARG, RTC_ERR_INIT_TIMEOUT, or
 *         RTC_ERR_SYNC_TIMEOUT.
 *
 * @note Threading: task-context only, non-blocking (≤ 4 ms worst case). NOT
 *       ISR-safe. NOT internally serialised.
 * @note Pre-condition (debug builds): rtc_init() has returned OK.
 */
rtc_err_t rtc_set_time(const rtc_datetime_t *dt);

/**
 * @brief Return whether the backup domain survived the last reset.
 *
 * Returns true if RTC_ISR.INITS was set at rtc_init() time. The flag is
 * captured once and is immutable for the module's lifetime — calling
 * rtc_set_time() does not flip it.
 *
 * @return true if backup domain was valid at init; false otherwise.
 * @note Threading: task-context only, non-blocking. NOT ISR-safe.
 * @note Pre-condition (debug builds): rtc_init() has returned OK.
 */
bool rtc_is_backup_valid(void);

/**
 * @brief Read one RTC backup register.
 *
 * Backup registers survive warm resets (watchdog, soft reset, NRST pin) as
 * long as VBAT or VDD supplies the backup domain. They do not survive a full
 * power-off on the Discovery boards (VBAT tied to VDD).
 *
 * DBP is set by rtc_init() and remains set; callers do not need to manage it.
 *
 * @param idx     Backup register index. Valid range: 0..19 on F469, 0..31
 *                on L475. Out-of-range returns RTC_ERR_BACKUP_BOUNDS.
 * @param[out] out Set to the 32-bit register value on RTC_OK. Untouched on error.
 * @return RTC_OK, RTC_ERR_NULL_ARG, or RTC_ERR_BACKUP_BOUNDS.
 *
 * @note Threading: task-context only, non-blocking. NOT ISR-safe.
 * @note Pre-condition (debug builds): rtc_init() has returned OK.
 */
rtc_err_t rtc_read_backup(uint8_t idx, uint32_t *out);

/**
 * @brief Write one RTC backup register.
 *
 * Same DBP and VBAT constraints as rtc_read_backup(). Backup registers are
 * not protected by WPR (which only covers TR/DR/CR).
 *
 * @param idx   Backup register index. Range: 0..19 on F469, 0..31 on L475.
 * @param value 32-bit value to write.
 * @return RTC_OK or RTC_ERR_BACKUP_BOUNDS.
 *
 * @note Threading: task-context only, non-blocking. NOT ISR-safe.
 * @note Pre-condition (debug builds): rtc_init() has returned OK.
 */
rtc_err_t rtc_write_backup(uint8_t idx, uint32_t value);

/**
 * @brief Override the tick source used for INITF/RSF poll timeouts.
 *
 * The driver samples a monotonic tick counter to bound the INITF and RSF
 * polling loops. Passing NULL restores the internal default, which returns 0
 * — making the timeout mechanism a no-op (unbounded wait). Production code
 * calls this once during startup with a SysTick-based millisecond reader.
 * Tests inject a controllable counter to drive timeout error paths.
 *
 * Project-wide idiom for environmental-dependency injection (DebugUartDriver
 * established the pattern).
 *
 * @param fn Tick-reader function, or NULL to restore the default.
 *
 * @note Threading: set once during system init. Not safe to change while
 *       another driver call is in flight.
 */
void rtc_set_tick_source(rtc_tick_source_fn fn);

/* --------------------------------------------------------------------- */
/* Singleton vtable (LLD-D10)                                             */
/* --------------------------------------------------------------------- */

/**
 * @brief Vtable exposing the RtcDriver public surface.
 *
 * Logger and TimeProvider depend on this interface; the concrete driver is
 * injected as a const pointer to a single static instance.
 */
typedef struct
{
    rtc_err_t (*init)(void);
    rtc_err_t (*get_time)(rtc_datetime_t *dt);
    rtc_err_t (*set_time)(const rtc_datetime_t *dt);
    bool      (*is_backup_valid)(void);
    rtc_err_t (*read_backup)(uint8_t idx, uint32_t *out);
    rtc_err_t (*write_backup)(uint8_t idx, uint32_t value);
} irtc_t;

/** Singleton pointer to the RtcDriver vtable instance. */
extern const irtc_t * const rtc_driver;

/* --------------------------------------------------------------------- */
/* Test-only hooks (#ifdef TEST)                                          */
/* --------------------------------------------------------------------- */

#ifdef TEST
/**
 * @brief Reset module state for unit tests.
 *
 * Clears the internal s_rtc state to its post-bss value. Test-only.
 */
void rtc_reset_for_test(void);

/**
 * @brief Expose BCD helpers for TC-RTC-032 round-trip test (RTCD-O4).
 */
uint8_t rtc_bcd_to_bin(uint8_t bcd);
uint8_t rtc_bin_to_bcd(uint8_t bin);
#endif

#endif /* RTC_DRIVER_H */
