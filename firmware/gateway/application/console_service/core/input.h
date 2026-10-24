/**
 * @file input.h
 * @brief Serial byte stream to key events (escape-sequence parser).
 *
 * A small state machine over the raw bytes from the terminal: arrows (CSI and application
 * cursor mode), Enter, Backspace, Tab, printable characters, the lone Escape key, and the
 * terminal's window-size report. A lone ESC is told apart from the start of a sequence by a
 * 10 ms window (LLD §11.1).
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §11.1
 */

#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Key kinds produced by the decoder. */
typedef enum
{
    KEY_NONE = 0,
    KEY_UP,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_ESCAPE,
    KEY_DIGIT,      /**< '1'..'7'; ch holds the digit value 1..7.            */
    KEY_PRINTABLE,  /**< Any other 0x20..0x7E character; ch holds it.        */
    KEY_REDRAW,     /**< Ctrl-L: repaint the whole screen.                    */
    KEY_SIZE_REPORT /**< Terminal size report; rows / cols are filled in.     */
} key_kind_t;

/** @brief One decoded key. */
typedef struct
{
    key_kind_t kind;
    uint8_t ch;
    uint16_t rows;
    uint16_t cols;
} key_event_t;

/** Window after a lone ESC before it is reported as the Escape key. */
#define INPUT_ESC_TIMEOUT_MS (10U)

/** @brief Forget any half-received sequence. */
void input_reset(void);

/**
 * @brief Feed one received byte.
 *
 * @param[in]  byte    Received byte.
 * @param[in]  now_ms  Current time, used to time the lone-ESC window.
 * @param[out] out     Receives the key when this byte completes one.
 * @return true if @p out holds a key.
 */
bool input_feed(uint8_t byte, uint32_t now_ms, key_event_t *out);

/**
 * @brief Report a pending lone ESC as KEY_ESCAPE once its window has passed.
 *
 * Call when no byte arrived for a while.
 *
 * @return true if @p out holds KEY_ESCAPE.
 */
bool input_poll_timeout(uint32_t now_ms, key_event_t *out);

/**
 * @brief Take the key that followed an Escape key in the same burst, if there is one.
 *
 * When ESC is followed at once by a byte that does not continue a sequence, input_feed()
 * reports the Escape and keeps that byte; call this after every key to receive it.
 *
 * @return true if @p out holds a key (a byte that decodes to nothing yields false).
 */
bool input_take_pending(key_event_t *out);

/** @brief Whether a lone ESC is waiting for its window to expire (needs a timed wake-up). */
bool input_esc_pending(void);

#endif /* INPUT_H */
