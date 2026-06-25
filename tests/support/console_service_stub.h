/**
 * @file console_service_stub.h
 * @brief Narrow stub for IConsoleService in LifecycleController unit tests.
 *
 * Provides iconsole_service_t with init_finalise() and run_once() — the
 * only ConsoleService methods LifecycleController calls. Prevents Ceedling
 * from auto-linking console_service.c (which cascades to debug_uart → CMSIS).
 *
 * Basename: console_service_stub — does NOT match console_service.c.
 */

#ifndef CONSOLE_SERVICE_STUB_H
#define CONSOLE_SERVICE_STUB_H

typedef enum
{
    CONSOLE_SERVICE_ERR_OK        = 0,
    CONSOLE_SERVICE_ERR_NULL_ARG  = 1,
    CONSOLE_SERVICE_ERR_NOT_INIT  = 2,
} console_service_err_t;

typedef struct
{
    console_service_err_t (*init_finalise)(void);
    console_service_err_t (*run_once)(void);
} iconsole_service_t;

#endif /* CONSOLE_SERVICE_STUB_H */
