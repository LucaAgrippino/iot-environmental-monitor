/**
 * @file main_test_update_service.c
 * @brief Hardware bring-up test for UpdateService (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default. Written against docs/lld/application/update-service-lld.md §23.1.
 * Real FreeRTOS, real FirmwareStore, InternalFlashDriver and QspiFlashDriver; the transport, the
 * probes and the lifecycle are harness-local stand-ins. The harness plays LifecycleController's
 * part: it starts the update when UpdateService asks, resets the board after `activate`, and
 * rolls back when the self-check fails.
 *
 * It needs the image linked for a slot and the bootloader (scripts/build-bootloader.sh) or the
 * boot stub behind it, exactly like the FirmwareStore harness (main_test_firmware_store.c):
 *   - in firmware/gateway/.cproject (bench-local, do not commit): make this folder the active
 *     harness, un-exclude firmware_store and update_service, enable MBEDTLS_ECDSA_C;
 *   - provision a release key (tools/ota/make_image.py genkey) and build both slot images;
 *   - scripts/build-slot-a.sh links the image for Slot A; flash the bootloader first.
 *
 * DESTRUCTIVE. Erases and programs the on-chip metadata and the inactive slot, and uses the QSPI
 * OTA staging area. Never touches the running slot or the bootloader.
 *
 * Host side: scripts/ota_uart_server.py. USART1 TX PB6, RX PB7, 115 200 8N1.
 *
 * Protocol, one command per line, host -> device:
 *   CMD <job> <size_a> <size_b>        deliver an OTA command (IUpdateService.handle_command)
 *   DATA <job> <offset> <len>          followed by <len> raw bytes: one chunk (on_chunk)
 *   LINK <0|1>                         the transport's is_connected() answer
 *   PROBE <sensors 0|1> <modbus 0|1>   the self-check probes
 *   SEED <T|R>                         write a TRIAL_PENDING / ROLLED_BACK record, then reset
 *   INFO                               print the boot info
 * device -> host:
 *   REQ <job> <A|B> <offset> <len>     a chunk request
 *   REPORT <result> <reason> <job|-> <version hex> <rollbacks>
 *   EVENT <name> <param>               a lifecycle event UpdateService posted
 *   and free text lines (INFO, results of commands).
 *
 * Test sequence (TC-HW-US-nnn from §23.1) is driven by scripts/ota_uart_server.py:
 *   001   valid max-size Slot-B image: accepted, every chunk, verify, install, activate, reset.
 *   002   the server stops answering: three retries, failed/download_failed.
 *   003   LINK 0 for 2 minutes, then LINK 1: no failure counted, resumes at the same offset.
 *   004   power cut mid-download, send the same job again: resumes <= 4 KB before the cut.
 *   005   one flipped body byte: failed/hash_mismatch.
 *   006   an image linked for the other slot: failed/wrong_slot.
 *   007   a second CMD during the download: rejected/busy, the first update completes.
 *   009   SEED T: the bootloader promotes TRIAL_PENDING to TRIAL_RUNNING and the harness resumes
 *         the self-check; success, CONFIRMED read back. (A TRIAL_RUNNING record at reset would be
 *         an unconfirmed trial, and the bootloader would roll it back.)
 *   010   SEED R: after the reset rolled_back is reported and acknowledged, CONFIRMED read back.
 *   008   (gap meter, stack high-water mark) is not in this harness: see US-O6 and FS-O10.
 *
 * BENCH_SELFCHECK_FAIL: build with -DBENCH_SELFCHECK_FAIL and the sensors probe never becomes
 * ready, so an image built this way fails its self-check and is rolled back (TC-SYS-OTA-002, the
 * REQ-NF-204 timing).
 *
 * Integration checklist:
 *   [ ] Every host command ends in the expected report
 *   [ ] Record: download time, chunk round trip, install time
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "config_store/config_store_crc.h"
#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "firmware_store/firmware_image_format.h"
#include "firmware_store/firmware_store.h"
#include "gpio/gpio_driver.h"
#include "internal_flash/iinternal_flash.h"
#include "internal_flash/internal_flash.h"
#include "qspi_flash/qspi_flash.h"
#include "task.h"
#include "update_service/update_service.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

#define BRINGUP_UART_TX_PORT (GPIO_PORT_B)
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT (GPIO_PORT_B)
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)

#define UART_SEND_TIMEOUT_MS (200U)
#define RX_RING_SIZE (2048U)
#define LINE_MAX_LEN (96U)
#define RESET_GRACE_MS (2000U)
#define BENCH_TASK_STACK_WORDS (512U)
#define BENCH_TASK_PRIORITY (2U)
#define BENCH_POLL_MS (5U)
#define RAW_WAIT_MS (2000U)

/* ---------------------------------------------------------------------- */
/* Console and the receive ring (filled by the UART interrupt)             */
/* ---------------------------------------------------------------------- */

static volatile uint8_t s_rx[RX_RING_SIZE];
static volatile uint32_t s_rx_head;
static volatile uint32_t s_rx_tail;

static void rx_byte_isr(uint8_t byte, void *context)
{
    (void) context;

    const uint32_t next = (s_rx_head + 1U) % RX_RING_SIZE;

    if (next != s_rx_tail)
    {
        s_rx[s_rx_head] = byte;
        s_rx_head = next;
    }
}

static bool rx_pop(uint8_t *out)
{
    if (s_rx_tail == s_rx_head)
    {
        return false;
    }

    *out = s_rx[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1U) % RX_RING_SIZE;

    return true;
}

static void say(const char *text)
{
    (void) debug_uart_send((const uint8_t *) text, strlen(text), UART_SEND_TIMEOUT_MS);
}

static void say_u(const char *fmt, unsigned long a)
{
    char line[LINE_MAX_LEN + 32U];

    (void) snprintf(line, sizeof(line), fmt, a);
    say(line);
}

static void say_uu(const char *fmt, unsigned long a, unsigned long b)
{
    char line[LINE_MAX_LEN + 32U];

    (void) snprintf(line, sizeof(line), fmt, a, b);
    say(line);
}

/* ---------------------------------------------------------------------- */
/* Harness-local stand-ins                                                  */
/* ---------------------------------------------------------------------- */

static volatile bool s_link_up = true;
#ifdef BENCH_SELFCHECK_FAIL
static volatile bool s_sensors = false; /* this image can never pass its self-check */
#else
static volatile bool s_sensors = true;
#endif
static volatile bool s_modbus = true;

/** Set by the lifecycle stand-in (UpdateServiceTask context), acted on by the bench task. */
static volatile bool s_start_pending;
static volatile uint32_t s_start_job;
static volatile bool s_reset_pending;
static volatile bool s_rollback_pending;

static bool t_is_connected(void *ctx)
{
    (void) ctx;

    return s_link_up;
}

static bool t_request_chunk(void *ctx, const us_chunk_request_t *req)
{
    (void) ctx;
    char line[LINE_MAX_LEN];

    (void) snprintf(line, sizeof(line), "REQ %lu %c %lu %lu\r\n", (unsigned long) req->job,
                    (req->slot == FIRMWARE_SLOT_B) ? 'B' : 'A', (unsigned long) req->offset,
                    (unsigned long) req->len);
    say(line);

    return true;
}

static const char *const k_result_names[] = {"accepted", "rejected", "failed", "success",
                                             "rolled_back"};

static bool t_report(void *ctx, const us_ota_report_t *rep)
{
    (void) ctx;

    char line[LINE_MAX_LEN];

    if (rep->job_known)
    {
        (void) snprintf(
            line, sizeof(line), "REPORT %s %s %lu %08lX %lu\r\n", k_result_names[rep->result],
            (rep->reason == US_REASON_NONE) ? "-" : update_service_reason_string(rep->reason),
            (unsigned long) rep->job, (unsigned long) rep->version,
            (unsigned long) rep->rollback_count);
    }
    else
    {
        (void) snprintf(
            line, sizeof(line), "REPORT %s %s - %08lX %lu\r\n", k_result_names[rep->result],
            (rep->reason == US_REASON_NONE) ? "-" : update_service_reason_string(rep->reason),
            (unsigned long) rep->version, (unsigned long) rep->rollback_count);
    }

    say(line);

    return true;
}

static const iota_transport_t s_transport = {
    .ctx = NULL,
    .is_connected = t_is_connected,
    .request_chunk = t_request_chunk,
    .report = t_report,
};

static bool p_sensors(void *ctx)
{
    (void) ctx;

    return s_sensors;
}

static bool p_modbus(void *ctx)
{
    (void) ctx;

    return s_modbus;
}

static const us_probe_if_t s_probes = {
    .ctx = NULL,
    .sensors_ready = p_sensors,
    .modbus_ready = p_modbus,
};

static lifecycle_state_t l_get_state(void)
{
    return LIFECYCLE_STATE_OPERATIONAL;
}

static bool l_post_event(lifecycle_event_t event)
{
    say_uu("EVENT %lu %lu\r\n", (unsigned long) event.type, (unsigned long) event.param);

    switch (event.type)
    {
    case LC_EVENT_OTA_REQUESTED:
        s_start_job = event.param;
        s_start_pending = true; /* start() is called from the bench task: state is not set yet */
        break;
    case LC_EVENT_OTA_RESET_REQUESTED:
        s_reset_pending = true;
        break;
    case LC_EVENT_SELF_CHECK_FAIL:
        s_rollback_pending = true;
        break;
    default:
        break;
    }

    return true;
}

static const ilifecycle_t s_lifecycle = {
    .get_state = l_get_state,
    .post_event = l_post_event,
};

/* ---------------------------------------------------------------------- */
/* Boot record seeding (TC-HW-US-009, -010)                                 */
/* ---------------------------------------------------------------------- */

/** @brief Write a boot record of the given state to the copy that is not selected, then reset. */
static void seed_and_reset(uint8_t state)
{
    firmware_boot_meta_t best = {0};
    uint8_t best_copy = 0U;
    bool have = false;

    for (uint8_t c = 0U; c < 2U; c++)
    {
        firmware_boot_meta_t r;
        const uint32_t addr = (c == 0U) ? FIRMWARE_META_A_ADDR : FIRMWARE_META_B_ADDR;

        if ((internal_flash_driver->read(addr, (uint8_t *) &r, sizeof(r)) == INTERNAL_FLASH_OK) &&
            (r.magic == FIRMWARE_META_MAGIC) &&
            (r.crc32 == config_store_crc32((const uint8_t *) &r, FIRMWARE_META_CRC_SPAN)) &&
            (!have || (r.generation > best.generation)))
        {
            best = r;
            best_copy = c;
            have = true;
        }
    }

    const uint32_t target =
        (have && (best_copy == 0U)) ? FIRMWARE_META_B_ADDR : FIRMWARE_META_A_ADDR;
    firmware_boot_meta_t r = {
        .magic = FIRMWARE_META_MAGIC,
        .active_bank = have ? best.active_bank : (uint8_t) FIRMWARE_BANK_A,
        .boot_state = state,
        .previous_bank = have ? best.previous_bank : (uint8_t) FIRMWARE_BANK_B,
        .reserved = 0xFFU,
        .rollback_count = (state == FIRMWARE_META_STATE_ROLLED_BACK) ? 1U : 0U,
        .generation = have ? (best.generation + 1U) : 1U,
        .pad = 0xFFFFFFFFUL,
    };

    r.crc32 = config_store_crc32((const uint8_t *) &r, FIRMWARE_META_CRC_SPAN);

    if ((internal_flash_driver->erase_page(target) != INTERNAL_FLASH_OK) ||
        (internal_flash_driver->program(target, (const uint8_t *) &r, 16U) != INTERNAL_FLASH_OK) ||
        (internal_flash_driver->program(target + 16U, ((const uint8_t *) &r) + 16U, 8U) !=
         INTERNAL_FLASH_OK))
    {
        say("SEED failed\r\n");
        return;
    }

    say("SEED ok, resetting\r\n");
    vTaskDelay(pdMS_TO_TICKS(100U));
    NVIC_SystemReset();
}

/* ---------------------------------------------------------------------- */
/* The bench task: command lines, and LifecycleController's part            */
/* ---------------------------------------------------------------------- */

static update_service_t *s_us;
static firmware_store_t *s_fs;

static void print_info(void)
{
    firmware_boot_info_t b;

    if (firmware_store_boot_ops.get_boot_info(s_fs, &b) == FIRMWARE_STORE_ERR_OK)
    {
        char line[LINE_MAX_LEN];

        (void) snprintf(line, sizeof(line), "INFO capable=%lu slot=%lu state=%lu version=%08lX\r\n",
                        (unsigned long) b.ota_capable, (unsigned long) b.running_slot,
                        (unsigned long) b.state, (unsigned long) b.running_version);
        say(line);
    }
}

/** @brief Read exactly @p len raw bytes into @p buf; false on a timeout. */
static bool read_raw(uint8_t *buf, uint32_t len)
{
    uint32_t got = 0U;
    uint32_t idle_ms = 0U;

    while (got < len)
    {
        uint8_t b;

        if (rx_pop(&b))
        {
            buf[got] = b;
            got++;
            idle_ms = 0U;
        }
        else
        {
            vTaskDelay(1U);
            idle_ms++;

            if (idle_ms > RAW_WAIT_MS)
            {
                return false;
            }
        }
    }

    return true;
}

static void handle_line(const char *line)
{
    unsigned long a = 0UL;
    unsigned long b = 0UL;
    unsigned long c = 0UL;

    if (sscanf(line, "CMD %lu %lu %lu", &a, &b, &c) == 3)
    {
        const us_ota_command_t cmd = {
            .job = (uint32_t) a, .size_a = (uint32_t) b, .size_b = (uint32_t) c};

        say_u("CMD -> %lu\r\n", (unsigned long) update_service_ops.handle_command(s_us, &cmd));
    }
    else if (sscanf(line, "DATA %lu %lu %lu", &a, &b, &c) == 3)
    {
        static uint8_t chunk[US_CHUNK_SIZE];

        if ((c == 0UL) || (c > US_CHUNK_SIZE) || !read_raw(chunk, (uint32_t) c))
        {
            say("DATA bad\r\n");
            return;
        }

        const update_service_err_t rc =
            update_service_ops.on_chunk(s_us, (uint32_t) a, (uint32_t) b, chunk, (uint32_t) c);

        if (rc != UPDATE_SERVICE_ERR_OK)
        {
            say_u("DATA -> %lu\r\n", (unsigned long) rc);
        }
    }
    else if (sscanf(line, "LINK %lu", &a) == 1)
    {
        s_link_up = (a != 0UL);
    }
    else if (sscanf(line, "PROBE %lu %lu", &a, &b) == 2)
    {
        s_sensors = (a != 0UL);
        s_modbus = (b != 0UL);
    }
    else if (strncmp(line, "SEED T", 6U) == 0)
    {
        seed_and_reset(FIRMWARE_META_STATE_TRIAL_PENDING);
    }
    else if (strncmp(line, "SEED R", 6U) == 0)
    {
        seed_and_reset(FIRMWARE_META_STATE_ROLLED_BACK);
    }
    else if (strncmp(line, "INFO", 4U) == 0)
    {
        print_info();
    }
    else
    {
        say("? \r\n");
    }
}

/** @brief What LifecycleController does with UpdateService's events (Machine 1). */
static void act_as_lifecycle(void)
{
    if (s_start_pending)
    {
        s_start_pending = false;
        say_uu("start(%lu) -> %lu\r\n", (unsigned long) s_start_job,
               (unsigned long) update_control_ops.start(s_us, s_start_job));
    }

    if (s_rollback_pending)
    {
        s_rollback_pending = false;
        (void) update_control_ops.resume_rollback(s_us);
        say_u("rollback -> %lu\r\n", (unsigned long) firmware_store_boot_ops.rollback(s_fs));
        vTaskDelay(pdMS_TO_TICKS(100U));
        NVIC_SystemReset();
    }

    if (s_reset_pending)
    {
        s_reset_pending = false;
        say("reset in 2 s (LifecycleController I18)\r\n");
        vTaskDelay(pdMS_TO_TICKS(RESET_GRACE_MS));
        NVIC_SystemReset();
    }
}

/** @brief The boot-time decisions of Init: resume the self-check or the rollback report. */
static void resume_after_boot(void)
{
    firmware_boot_info_t b;

    if ((firmware_store_boot_ops.get_boot_info(s_fs, &b) != FIRMWARE_STORE_ERR_OK) ||
        !b.ota_capable)
    {
        return;
    }

    if (b.state == FIRMWARE_BOOT_TRIAL_RUNNING)
    {
        say_u("resume_self_checking -> %lu\r\n",
              (unsigned long) update_control_ops.resume_self_checking(s_us));
    }
    else if (b.state == FIRMWARE_BOOT_ROLLED_BACK)
    {
        say_u("resume_after_rollback -> %lu\r\n",
              (unsigned long) update_control_ops.resume_after_rollback(s_us));
    }
    else
    {
        /* CONFIRMED: nothing to resume. */
    }
}

static void bench_task(void *arg)
{
    (void) arg;

    char line[LINE_MAX_LEN];
    uint32_t len = 0U;

    resume_after_boot();

    for (;;)
    {
        uint8_t byte;

        act_as_lifecycle();

        if (!rx_pop(&byte))
        {
            vTaskDelay(pdMS_TO_TICKS(BENCH_POLL_MS));
            continue;
        }

        if ((byte == (uint8_t) '\n') || (byte == (uint8_t) '\r'))
        {
            if (len > 0U)
            {
                line[len] = '\0';
                handle_line(line);
                len = 0U;
            }
        }
        else if (len < (LINE_MAX_LEN - 1U))
        {
            line[len] = (char) byte;
            len++;
        }
        else
        {
            len = 0U; /* overlong line: drop it */
        }
    }
}

static StaticTask_t s_bench_tcb;
static StackType_t s_bench_stack[BENCH_TASK_STACK_WORDS];

/* ---------------------------------------------------------------------- */
/* main                                                                    */
/* ---------------------------------------------------------------------- */

int main(void)
{
    if (cpu_init() != STATUS_OK)
    {
        for (;;)
        {
            /* Nothing can be reported before the UART exists. */
        }
    }

    gpio_pin_config_t uart_tx_config = {
        .port = BRINGUP_UART_TX_PORT,
        .pin = BRINGUP_UART_TX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_NONE,
        .alternate = BRINGUP_UART_AF,
    };
    gpio_pin_config_t uart_rx_config = {
        .port = BRINGUP_UART_RX_PORT,
        .pin = BRINGUP_UART_RX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_UP,
        .alternate = BRINGUP_UART_AF,
    };

    if ((gpio_init() != GPIO_OK) || (gpio_configure_pin(&uart_tx_config) != GPIO_OK) ||
        (gpio_configure_pin(&uart_rx_config) != GPIO_OK) || (debug_uart_init() != DEBUG_UART_OK) ||
        (debug_uart_attach_rx_byte(rx_byte_isr, NULL) != DEBUG_UART_OK))
    {
        for (;;)
        {
            /* No sink to report to. */
        }
    }

    say("\r\n===== UpdateService Hardware Bring-up =====\r\n");
    say("Board  : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    say("WARNING: erases the on-chip metadata and the inactive slot\r\n");
    say("===========================================\r\n");

    if ((qspi_flash_init() != QSPI_FLASH_OK) || (internal_flash_init() != INTERNAL_FLASH_OK))
    {
        say("FATAL flash init\r\n");
        for (;;)
        {
        }
    }

    const firmware_store_config_t fs_cfg = {
        .qspi = qspi_flash_driver,
        .iflash = internal_flash_driver,
        .running_image_base = cpu_get_vector_table_base(),
    };

    if (firmware_store_create(&fs_cfg, &s_fs) != FIRMWARE_STORE_ERR_OK)
    {
        say("FATAL firmware_store_create\r\n");
        for (;;)
        {
        }
    }

    const update_service_config_t us_cfg = {
        .fs_image = &firmware_store_image_ops,
        .fs_boot = &firmware_store_boot_ops,
        .fs = s_fs,
        .transport = &s_transport,
        .probes = &s_probes,
        .lifecycle = &s_lifecycle,
    };

    if (update_service_create(&us_cfg, &s_us) != UPDATE_SERVICE_ERR_OK)
    {
        say("FATAL update_service_create\r\n");
        for (;;)
        {
        }
    }

    print_info();

    if (xTaskCreateStatic(bench_task, "UsBench", BENCH_TASK_STACK_WORDS, NULL, BENCH_TASK_PRIORITY,
                          s_bench_stack, &s_bench_tcb) == NULL)
    {
        say("FATAL bench task\r\n");
    }

    vTaskStartScheduler();

    for (;;)
    {
        /* Not reached. */
    }
}
