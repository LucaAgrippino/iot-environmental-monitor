/**
 * @file fake_fs_ops.c
 * @brief Scriptable FirmwareStore vtable fakes (see fake_fs_ops.h).
 */

#include "fake_fs_ops.h"

#include <string.h>

#include "fake_log.h"

typedef struct
{
    firmware_store_err_t script[FFS_SCRIPT_MAX];
    uint32_t script_len;
    uint32_t calls;
    firmware_store_err_t always;
} fn_state_t;

static fn_state_t s_fn[FFS_COUNT];
static fake_fs_fn_t s_log[FFS_LOG_MAX];
static uint32_t s_log_count;

static uint32_t s_resume_offset;
static uint32_t s_verify_version;
static uint32_t s_verify_len;
static firmware_slot_t s_verify_slot;
static uint32_t s_install_steps;
static uint32_t s_install_step_calls;
static firmware_boot_info_t s_boot;
static uint32_t s_write_offset;
static uint32_t s_write_len;
static uint8_t s_write_data[FFS_WRITE_KEEP];
static uint32_t s_last_job;
static uint32_t s_last_file_len;

static const char *const k_names[FFS_COUNT] = {
    "begin",    "resume",  "write_chunk",   "verify",  "install_begin", "install_step",
    "activate", "discard", "get_boot_info", "confirm", "ack_rollback",
};

/** @brief Common entry: audit the mutex, trace, log, count, and return the scripted code. */
static firmware_store_err_t enter(fake_fs_fn_t fn)
{
    fake_audit_mutex();
    fake_trace_add(k_names[fn]);

    if (s_log_count < FFS_LOG_MAX)
    {
        s_log[s_log_count] = fn;
        s_log_count++;
    }

    fn_state_t *const f = &s_fn[fn];
    const uint32_t call = f->calls;

    f->calls++;

    return (call < f->script_len) ? f->script[call] : f->always;
}

static firmware_store_err_t f_begin(firmware_store_t *self, uint32_t image_id, uint32_t file_len)
{
    (void) self;
    s_last_job = image_id;
    s_last_file_len = file_len;

    return enter(FFS_BEGIN);
}

static firmware_store_err_t f_resume(firmware_store_t *self, uint32_t image_id, uint32_t file_len,
                                     uint32_t *offset_out)
{
    (void) self;
    s_last_job = image_id;
    s_last_file_len = file_len;

    const firmware_store_err_t rc = enter(FFS_RESUME);

    if ((rc == FIRMWARE_STORE_ERR_OK) && (offset_out != NULL))
    {
        *offset_out = s_resume_offset;
    }

    return rc;
}

static firmware_store_err_t f_write_chunk(firmware_store_t *self, uint32_t offset,
                                          const uint8_t *data, uint32_t len)
{
    (void) self;
    s_write_offset = offset;
    s_write_len = len;
    (void) memcpy(s_write_data, data, (len < FFS_WRITE_KEEP) ? len : FFS_WRITE_KEEP);

    return enter(FFS_WRITE_CHUNK);
}

static firmware_store_err_t f_verify(firmware_store_t *self, firmware_image_info_t *info_out)
{
    (void) self;

    const firmware_store_err_t rc = enter(FFS_VERIFY);

    if ((rc == FIRMWARE_STORE_ERR_OK) && (info_out != NULL))
    {
        info_out->version = s_verify_version;
        info_out->image_len = s_verify_len;
        info_out->target_slot = s_verify_slot;
    }

    return rc;
}

static firmware_store_err_t f_install_begin(firmware_store_t *self)
{
    (void) self;
    s_install_step_calls = 0U;

    return enter(FFS_INSTALL_BEGIN);
}

static firmware_store_err_t f_install_step(firmware_store_t *self, bool *done_out)
{
    (void) self;

    const firmware_store_err_t rc = enter(FFS_INSTALL_STEP);

    s_install_step_calls++;
    *done_out = (s_install_step_calls >= s_install_steps);

    return rc;
}

static firmware_store_err_t f_activate(firmware_store_t *self)
{
    (void) self;

    return enter(FFS_ACTIVATE);
}

static firmware_store_err_t f_discard(firmware_store_t *self)
{
    (void) self;

    return enter(FFS_DISCARD);
}

static firmware_store_err_t f_get_boot_info(const firmware_store_t *self,
                                            firmware_boot_info_t *info_out)
{
    (void) self;

    const firmware_store_err_t rc = enter(FFS_GET_BOOT_INFO);

    if ((rc == FIRMWARE_STORE_ERR_OK) && (info_out != NULL))
    {
        *info_out = s_boot;
    }

    return rc;
}

static firmware_store_err_t f_confirm(firmware_store_t *self)
{
    (void) self;

    return enter(FFS_CONFIRM);
}

static firmware_store_err_t f_ack_rollback(firmware_store_t *self)
{
    (void) self;

    return enter(FFS_ACK_ROLLBACK);
}

const ifirmware_image_store_t fake_fs_image_ops = {
    .begin = f_begin,
    .resume = f_resume,
    .write_chunk = f_write_chunk,
    .verify = f_verify,
    .install_begin = f_install_begin,
    .install_step = f_install_step,
    .activate = f_activate,
    .discard = f_discard,
};

const ifirmware_boot_control_t fake_fs_boot_ops = {
    .get_boot_info = f_get_boot_info,
    .confirm = f_confirm,
    .acknowledge_rollback = f_ack_rollback,
};

void fake_fs_reset(void)
{
    (void) memset(s_fn, 0, sizeof(s_fn));
    (void) memset(s_log, 0, sizeof(s_log));
    s_log_count = 0U;
    s_resume_offset = 0U;
    s_verify_version = 0x02000000UL;
    s_verify_len = 4096U;
    s_verify_slot = FIRMWARE_SLOT_B;
    s_install_steps = 3U;
    s_install_step_calls = 0U;
    s_write_offset = 0U;
    s_write_len = 0U;
    s_last_job = 0U;
    s_last_file_len = 0U;
    (void) memset(s_write_data, 0, sizeof(s_write_data));

    s_boot = (firmware_boot_info_t){
        .ota_capable = true,
        .running_slot = FIRMWARE_SLOT_A,
        .state = FIRMWARE_BOOT_CONFIRMED,
        .rollback_count = 0U,
        .running_version = 0x01000000UL,
    };
}

void fake_fs_script(fake_fs_fn_t fn, const firmware_store_err_t *codes, uint32_t n)
{
    fn_state_t *const f = &s_fn[fn];

    f->script_len = (n < FFS_SCRIPT_MAX) ? n : FFS_SCRIPT_MAX;
    (void) memcpy(f->script, codes, f->script_len * sizeof(codes[0]));
    f->calls = 0U;
}

void fake_fs_always(fake_fs_fn_t fn, firmware_store_err_t code)
{
    s_fn[fn].always = code;
}

uint32_t fake_fs_calls(fake_fs_fn_t fn)
{
    return s_fn[fn].calls;
}

uint32_t fake_fs_log_count(void)
{
    return s_log_count;
}

fake_fs_fn_t fake_fs_log_at(uint32_t index)
{
    return s_log[index];
}

void fake_fs_set_resume_offset(uint32_t offset)
{
    s_resume_offset = offset;
}

void fake_fs_set_verify_info(uint32_t version, uint32_t len, firmware_slot_t slot)
{
    s_verify_version = version;
    s_verify_len = len;
    s_verify_slot = slot;
}

void fake_fs_set_install_steps(uint32_t steps)
{
    s_install_steps = steps;
}

void fake_fs_set_boot_info(const firmware_boot_info_t *info)
{
    s_boot = *info;
}

uint32_t fake_fs_last_write_offset(void)
{
    return s_write_offset;
}

uint32_t fake_fs_last_write_len(void)
{
    return s_write_len;
}

const uint8_t *fake_fs_last_write_data(void)
{
    return s_write_data;
}

uint32_t fake_fs_last_job(void)
{
    return s_last_job;
}

uint32_t fake_fs_last_file_len(void)
{
    return s_last_file_len;
}
