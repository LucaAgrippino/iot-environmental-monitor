/**
 * @file header.c
 * @brief Header line composition.
 */

#include "header.h"

#include <stdio.h>

#include "firmware_version.h"
#include "theme.h"

#define DOT "\xE2\x97\x8F" /* ● */

/* A coloured "label ●" cell; unknown data shows a muted "?" instead of a dot. */
static int prv_cell(char *buf, size_t size, const char *label, bool known, bool good,
                    bool bad_is_err)
{
    if (!known)
    {
        return snprintf(buf, size, "%s%s ?%s", theme_sgr(PAL_MUTED), label, THEME_RESET);
    }
    return snprintf(buf, size, "%s%s " DOT "%s",
                    theme_sgr(good ? PAL_OK : (bad_is_err ? PAL_ERR : PAL_WARN)), label,
                    THEME_RESET);
}

void header_compose(const console_model_t *m, char *line1, size_t size1, char *line2, size_t size2)
{
    char up[24];
    char wifi[48];
    char mqtt[48];
    char mb[48];
    const uint8_t alarms = m->alarms_ok ? model_alarm_count(m) : 0U;

    model_uptime_short(m, up, sizeof(up));
    (void) snprintf(line1, size1, " %sENVMON GATEWAY%s  fw %s  up %s  %s%s%s", theme_sgr(PAL_NUM),
                    THEME_RESET, FW_VERSION_STRING, up,
                    theme_sgr(!m->alarms_ok ? PAL_MUTED : ((alarms > 0U) ? PAL_ERR : PAL_OK)),
                    !m->alarms_ok ? "alarms ?" : ((alarms > 0U) ? "ALARM" : "no alarms"),
                    THEME_RESET);
    (void) prv_cell(wifi, sizeof(wifi), "WiFi", m->health_ok, m->health.wifi_rssi_dbm < 0, true);
    (void) prv_cell(mqtt, sizeof(mqtt), "MQTT", m->health_ok, m->health.cloud_connected, true);
    (void) prv_cell(mb, sizeof(mb), "Modbus", m->health_ok, m->health.modbus_link_online, false);
    (void) snprintf(line2, size2, " %s  %s  %s  sensors %u/%u  rssi %ld dBm", wifi, mqtt, mb,
                    (unsigned) (m->sensors_ok ? model_sensors_online(m) : 0U), (unsigned) 4U,
                    (long) (m->health_ok ? m->health.wifi_rssi_dbm : 0));
}
