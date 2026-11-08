/**
 * @file commands.h
 * @brief Command table, handlers and the staged-change / confirm workflows.
 *
 * Commands read the live system through the injected services and write their result as lines
 * into the REPL scrollback. The side-effecting ones go through IConfigManager: `set` applies a
 * threshold at once, `config` and `prov` stage changes and apply them after a y/N confirm.
 * Commands whose back-end does not exist on the Gateway yet validate their input and report
 * "not available" (LLD §0 of the reconciliation).
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §9, §12, §13, §14
 */

#ifndef COMMANDS_H
#define COMMANDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console_service/console_types.h"

/** Confirm prompt lifetime, in 1 Hz data ticks (CS-O7). */
#define CONFIRM_TIMEOUT_TICKS (30U)

/** @brief Handler of one console command; argv[0] is the first argument after the command token. */
typedef console_service_err_t (*cmd_handler_t)(int argc, const char *argv[]);

/** @brief One entry of the command table (LLD §5.4). */
typedef struct
{
    const char *token;     /**< One or two words, e.g. "sensors" or "config set". */
    const char *alias;     /**< Optional alias for the whole token, or NULL.       */
    uint8_t min_argc;      /**< Arguments after the token.                         */
    uint8_t max_argc;      /**< Largest number of arguments after the token. */
    cmd_handler_t handler; /**< Function that runs the command. */
    const char *help;      /**< One-line help text shown by `help`. */
} cmd_entry_t;

/** @brief Selftest outcome: kept in RAM and stored with the configuration (REQ-LI-160). */
typedef struct
{
    bool valid;         /**< True when the self-test has run and the fields below are valid. */
    bool sensor_pass;   /**< Sensor self-check passed. */
    bool comms_pass;    /**< Communication self-check passed. */
    bool flash_checked; /**< True when the flash check was performed. */
    bool flash_pass;    /**< Flash check passed (meaningful only when flash_checked). */
    bool overall_pass;  /**< All checks that ran passed. */
    uint32_t uptime_s;  /**< Uptime when the self-test ran, in seconds. */
    bool from_store;    /**< Read back from the stored configuration (an earlier boot) */
} selftest_result_t;

/**
 * @brief Reset all command state and remember the injected services.
 *
 * @param deps Services the commands use; must outlive the console.
 */
void commands_init(const console_deps_t *deps);

/**
 * @brief Find and run a command.
 *
 * @param[in] argc  Number of words, at least 1.
 * @param[in] argv  The words; the command token is one or two of them.
 * @return The handler's result; CONSOLE_SERVICE_ERR_UNKNOWN_KEY if no command matches;
 *         CONSOLE_SERVICE_ERR_VALIDATION if the argument count is wrong.
 */
console_service_err_t commands_dispatch(int argc, const char *argv[]);

/**
 * @brief The command table and its length.
 *
 * @param count Receives the number of entries.
 * @return First entry of the table.
 */
const cmd_entry_t *commands_table(size_t *count);

/**
 * @brief Whether a y/N prompt is waiting for an answer.
 *
 * @return true while a prompt waits.
 */
bool commands_confirm_active(void);

/**
 * @brief True when the pending y/N prompt asks whether to reboot.
 *
 * @return true if the prompt is the reboot confirmation.
 */
bool commands_confirm_is_reboot(void);

/**
 * @brief Answer the prompt: apply the staged change (yes) or discard it (no).
 * @param yes true to apply the staged change, false to discard it.
 * @return The apply result; CONSOLE_SERVICE_OK for a decline.
 */
console_service_err_t commands_confirm_answer(bool yes);

/**
 * @brief 1 Hz tick: counts the prompt down and discards the staged change when it expires.
 * @return CONSOLE_SERVICE_ERR_TIMEOUT on the tick the prompt expired, else CONSOLE_SERVICE_OK.
 */
console_service_err_t commands_tick(void);

/**
 * @brief True once after a command changed the configuration and a flash write is due.
 *
 * @return true once per change; the flag is cleared by the call.
 */
bool commands_take_persist_due(void);

/**
 * @brief True once after `term` changed the layout size: the screen must be redrawn.
 *
 * @return true once per change; the flag is cleared by the call.
 */
bool commands_take_redraw_request(void);

/**
 * @brief True once after `reboot`; the service gives the terminal back and resets.
 *
 * @return true once per request; the flag is cleared by the call.
 */
bool commands_take_reboot_request(void);

/**
 * @brief Whether provisioning changes are staged.
 *
 * @return true if provisioning changes are staged.
 */
bool commands_prov_dirty(void);

/**
 * @brief Whether operational-config changes are staged.
 *
 * @return true if configuration changes are staged.
 */
bool commands_cfg_dirty(void);

/**
 * @brief The last self-test outcome.
 *
 * @return The result record; valid only when its `valid` flag is set.
 */
const selftest_result_t *commands_selftest_result(void);

#endif /* COMMANDS_H */
