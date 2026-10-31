/**
 * @file device_profile_registry.c
 * @brief DeviceProfileRegistry implementation (Gateway).
 *
 * One static table guarded by a mutex.  A writer changes a copy, saves the copy through the
 * storage callback and replaces the live table only when the save succeeded, so a reader can
 * never see a table that is not on flash and a failed save needs no rollback (LLD §6.2).
 *
 * @note Companion: docs/lld/application/device-profile-registry-lld.md v0.2
 */

#include "device_profile_registry.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

#ifdef TEST
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

#define MODULE_TAG "DPR"

#define SLAVE_ADDR_MIN (1U)
#define SLAVE_ADDR_MAX (247U)
#define PRINTABLE_MIN (0x20)
#define PRINTABLE_MAX (0x7E)

typedef struct
{
    device_profile_t profiles[DEVICE_PROFILE_MAX];
    uint8_t count;
    StaticSemaphore_t mutex_buf;
    StaticSemaphore_t write_mutex_buf;
    SemaphoreHandle_t mutex;       /* the live table and count                    */
    SemaphoreHandle_t write_mutex; /* one writer at a time, held across the save  */
    device_profile_storage_t storage;
    bool initialised;
} registry_t;

static registry_t s_reg;

/* ====================================================================== */
/* Helpers                                                                */
/* ====================================================================== */

static void lock(void)
{
    (void) xSemaphoreTake(s_reg.mutex, portMAX_DELAY);
}

static void unlock(void)
{
    (void) xSemaphoreGive(s_reg.mutex);
}

/** True if the description is NUL-terminated within its array and has a printable character. */
static bool description_valid(const char *d)
{
    for (uint8_t i = 0U; i < DEVICE_PROFILE_DESC_MAX; i++)
    {
        if (d[i] == '\0')
        {
            return i > 0U;
        }
        if (((int) d[i] < PRINTABLE_MIN) || ((int) d[i] > PRINTABLE_MAX))
        {
            return false;
        }
    }
    return false; /* no terminator */
}

static bool profile_valid(const device_profile_t *p)
{
    return (p->slave_addr >= SLAVE_ADDR_MIN) && (p->slave_addr <= SLAVE_ADDR_MAX) &&
           (p->map_version != 0U) && (p->vendor_code != 0U) && description_valid(p->description);
}

static void seed_default(void)
{
    (void) memset(&s_reg.profiles[0], 0, sizeof(s_reg.profiles[0]));
    s_reg.profiles[0].slave_addr = DEVICE_PROFILE_DEFAULT_ADDR;
    s_reg.profiles[0].map_version = DEVICE_PROFILE_DEFAULT_MAP_VERSION;
    s_reg.profiles[0].vendor_code = DEVICE_PROFILE_DEFAULT_VENDOR_CODE;
    (void) strncpy(s_reg.profiles[0].description, DEVICE_PROFILE_DEFAULT_DESCRIPTION,
                   DEVICE_PROFILE_DESC_MAX - 1U);
    s_reg.count = 1U;
}

/** Index of an address in a table, or count if absent. */
static uint8_t find_in(const device_profile_t *table, uint8_t count, uint8_t slave_addr)
{
    uint8_t i = 0U;

    while ((i < count) && (table[i].slave_addr != slave_addr))
    {
        i++;
    }
    return i;
}

/**
 * Save a candidate table and, only if that worked, make it the live one.  The caller holds
 * write_mutex.
 */
static device_profile_registry_err_t commit(const device_profile_t *candidate, uint8_t count)
{
    if (!s_reg.storage.save(candidate, count))
    {
        LOG_WARN(MODULE_TAG, "save failed, %u profile(s) not changed", (unsigned) s_reg.count);
        return DEVICE_PROFILE_REGISTRY_ERR_PERSIST;
    }

    lock();
    (void) memcpy(s_reg.profiles, candidate, sizeof(s_reg.profiles));
    s_reg.count = count;
    unlock();
    return DEVICE_PROFILE_REGISTRY_OK;
}

/* ====================================================================== */
/* Provider                                                               */
/* ====================================================================== */

static bool provider_get_next(device_profile_t *out, uint8_t *index)
{
    if ((out == NULL) || (index == NULL) || !s_reg.initialised)
    {
        return false;
    }

    lock();
    const bool have = (*index < s_reg.count);

    if (have)
    {
        *out = s_reg.profiles[*index];
        (*index)++;
    }
    unlock();
    return have;
}

static device_profile_registry_err_t provider_get_by_addr(uint8_t slave_addr, device_profile_t *out)
{
    if (out == NULL)
    {
        return DEVICE_PROFILE_REGISTRY_ERR_NULL_ARG;
    }
    if (!s_reg.initialised)
    {
        return DEVICE_PROFILE_REGISTRY_ERR_NOT_INIT;
    }

    device_profile_registry_err_t rc = DEVICE_PROFILE_REGISTRY_ERR_NOT_FOUND;

    lock();
    const uint8_t i = find_in(s_reg.profiles, s_reg.count, slave_addr);

    if (i < s_reg.count)
    {
        *out = s_reg.profiles[i];
        rc = DEVICE_PROFILE_REGISTRY_OK;
    }
    unlock();
    return rc;
}

static bool provider_is_registered(uint8_t slave_addr)
{
    device_profile_t unused;

    return provider_get_by_addr(slave_addr, &unused) == DEVICE_PROFILE_REGISTRY_OK;
}

static const idevice_profile_provider_t k_provider = {
    .get_next = provider_get_next,
    .get_by_addr = provider_get_by_addr,
    .is_registered = provider_is_registered,
};

const idevice_profile_provider_t *const device_profile_provider = &k_provider;

/* ====================================================================== */
/* Manager                                                                */
/* ====================================================================== */

static device_profile_registry_err_t manager_add_or_update(const device_profile_t *profile)
{
    if (profile == NULL)
    {
        return DEVICE_PROFILE_REGISTRY_ERR_NULL_ARG;
    }
    if (!s_reg.initialised)
    {
        return DEVICE_PROFILE_REGISTRY_ERR_NOT_INIT;
    }
    if (!profile_valid(profile))
    {
        LOG_WARN(MODULE_TAG, "profile for slave %u rejected", (unsigned) profile->slave_addr);
        return DEVICE_PROFILE_REGISTRY_ERR_INVALID;
    }

    device_profile_t candidate[DEVICE_PROFILE_MAX];
    uint8_t count;

    (void) xSemaphoreTake(s_reg.write_mutex, portMAX_DELAY);
    lock();
    (void) memcpy(candidate, s_reg.profiles, sizeof(candidate));
    count = s_reg.count;
    unlock();

    uint8_t i = find_in(candidate, count, profile->slave_addr);
    device_profile_registry_err_t rc = DEVICE_PROFILE_REGISTRY_OK;

    if ((i == count) && (count >= DEVICE_PROFILE_MAX))
    {
        LOG_WARN(MODULE_TAG, "registry full, slave %u not added", (unsigned) profile->slave_addr);
        rc = DEVICE_PROFILE_REGISTRY_ERR_FULL;
    }
    else
    {
        candidate[i] = *profile;
        if (i == count)
        {
            count++;
        }
        rc = commit(candidate, count);
    }
    (void) xSemaphoreGive(s_reg.write_mutex);
    return rc;
}

static device_profile_registry_err_t manager_remove(uint8_t slave_addr)
{
    if (!s_reg.initialised)
    {
        return DEVICE_PROFILE_REGISTRY_ERR_NOT_INIT;
    }

    device_profile_t candidate[DEVICE_PROFILE_MAX];
    uint8_t count;

    (void) xSemaphoreTake(s_reg.write_mutex, portMAX_DELAY);
    lock();
    (void) memcpy(candidate, s_reg.profiles, sizeof(candidate));
    count = s_reg.count;
    unlock();

    const uint8_t i = find_in(candidate, count, slave_addr);
    device_profile_registry_err_t rc = DEVICE_PROFILE_REGISTRY_ERR_NOT_FOUND;

    if (i < count)
    {
        count--;
        candidate[i] = candidate[count]; /* order is not significant */
        (void) memset(&candidate[count], 0, sizeof(candidate[count]));
        rc = commit(candidate, count);
    }
    (void) xSemaphoreGive(s_reg.write_mutex);
    return rc;
}

static const idevice_profile_manager_t k_manager = {
    .add_or_update = manager_add_or_update,
    .remove = manager_remove,
};

const idevice_profile_manager_t *const device_profile_manager = &k_manager;

/* ====================================================================== */
/* Lifecycle                                                              */
/* ====================================================================== */

device_profile_registry_err_t device_profile_registry_init(const device_profile_storage_t *storage)
{
    if ((storage == NULL) || (storage->load == NULL) || (storage->save == NULL))
    {
        return DEVICE_PROFILE_REGISTRY_ERR_NULL_ARG;
    }
    if (s_reg.initialised)
    {
        return DEVICE_PROFILE_REGISTRY_OK;
    }

    s_reg.mutex = xSemaphoreCreateMutexStatic(&s_reg.mutex_buf);
    s_reg.write_mutex = xSemaphoreCreateMutexStatic(&s_reg.write_mutex_buf);
    if ((s_reg.mutex == NULL) || (s_reg.write_mutex == NULL))
    {
        return DEVICE_PROFILE_REGISTRY_ERR_NO_RESOURCE;
    }

    s_reg.storage = *storage;
    s_reg.count = 0U;
    s_reg.initialised = true;
    return DEVICE_PROFILE_REGISTRY_OK;
}

device_profile_registry_err_t device_profile_registry_load(void)
{
    if (!s_reg.initialised)
    {
        return DEVICE_PROFILE_REGISTRY_ERR_NOT_INIT;
    }

    device_profile_t loaded[DEVICE_PROFILE_MAX];
    uint8_t n = 0U;
    bool ok =
        s_reg.storage.load(loaded, DEVICE_PROFILE_MAX, &n) && (n > 0U) && (n <= DEVICE_PROFILE_MAX);

    for (uint8_t i = 0U; ok && (i < n); i++)
    {
        ok = profile_valid(&loaded[i]) && (find_in(loaded, i, loaded[i].slave_addr) == i);
    }

    lock();
    if (ok)
    {
        (void) memset(s_reg.profiles, 0, sizeof(s_reg.profiles));
        (void) memcpy(s_reg.profiles, loaded, (size_t) n * sizeof(loaded[0]));
        s_reg.count = n;
    }
    else
    {
        seed_default();
    }
    unlock();

    if (ok)
    {
        LOG_INFO(MODULE_TAG, "loaded %u profile(s)", (unsigned) n);
    }
    else
    {
        LOG_WARN(MODULE_TAG, "no usable stored profiles, default seeded");
    }
    return DEVICE_PROFILE_REGISTRY_OK;
}

uint8_t device_profile_registry_count(void)
{
    return s_reg.initialised ? s_reg.count : 0U;
}

void device_profile_registry_reset_for_test(void)
{
    (void) memset(&s_reg, 0, sizeof(s_reg));
}

/* ====================================================================== */
/* Free-function form of the read side                                    */
/* ====================================================================== */

bool device_profile_get_next(device_profile_t *out, uint8_t *index)
{
    return provider_get_next(out, index);
}

bool device_profile_is_registered(uint8_t slave_addr)
{
    return provider_is_registered(slave_addr);
}
