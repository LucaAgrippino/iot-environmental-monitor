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

typedef console_service_err_t (*cmd_handler_t)(int argc, const char *argv[]);

/** @brief One entry of the command table (LLD §5.4). */
typedef struct
{
    const char *token; /**< One or two words, e.g. "sensors" or "config set". */
    const char *alias; /**< Optional alias for the whole token, or NULL.       */
    uint8_t min_argc;  /**< Arguments after the token.                         */
    uint8_t max_argc;
    cmd_handler_t handler;
    const char *help;
} cmd_entry_t;

/** @brief Selftest outcome kept in RAM (persistence is not available, CS-O5). */
typedef struct
{
    bool valid;
    bool sensor_pass;
    bool comms_pass;
    bool flash_checked;
    bool flash_pass;
    bool overall_pass;
    uint32_t uptime_s;
} selftest_result_t;

/** @brief Reset all command state and remember the injected services. */
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

/** @brief The command table and its length. */
const cmd_entry_t *commands_table(size_t *count);

/** @brief Whether a y/N prompt is waiting for an answer. */
bool commands_confirm_active(void);

/** @brief True when the pending y/N prompt asks whether to reboot. */
bool commands_confirm_is_reboot(void);

/** @brief Ask "Reboot?" (y/N, same timeout as the other prompts). Used by the Firmware panel. */
void commands_request_reboot_confirm(void);

/**
 * @brief Check for a firmware update. No update source exists on the Gateway yet, so this
 *        records and logs "not available"; the text stays readable through
 *        commands_fw_check_result().
 */
console_service_err_t commands_fw_check(void);

/** @brief Result line of the last update check, or NULL if none was run. */
const char *commands_fw_check_result(void);

/**
 * @brief Answer the prompt: apply the staged change (yes) or discard it (no).
 * @return The apply result; CONSOLE_SERVICE_OK for a decline.
 */
console_service_err_t commands_confirm_answer(bool yes);

/**
 * @brief 1 Hz tick: counts the prompt down and discards the staged change when it expires.
 * @return CONSOLE_SERVICE_ERR_TIMEOUT on the tick the prompt expired, else CONSOLE_SERVICE_OK.
 */
console_service_err_t commands_tick(void);

/** @brief True once after a command changed the configuration and a flash write is due. */
bool commands_take_persist_due(void);

/** @brief True once after `reboot`; the service tears the screen down and resets. */
bool commands_take_reboot_request(void);

/** @brief Whether provisioning changes are staged. */
bool commands_prov_dirty(void);

/** @brief Whether operational-config changes are staged. */
bool commands_cfg_dirty(void);

/** @brief The last self-test outcome. */
const selftest_result_t *commands_selftest_result(void);

#endif /* COMMANDS_H */
