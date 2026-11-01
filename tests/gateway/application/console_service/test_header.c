/**
 * @file test_header.c
 * @brief Unity tests for the header line composition.
 */

#include <stdio.h>
#include <string.h>

#include "header.h"
#include "theme.h"
#include "unity.h"

/* The header only needs three answers from the model; stubs keep the real model (and the
 * HealthMonitor it pulls from) out of this test. */
static uint8_t s_alarm_count;

uint8_t model_alarm_count(const console_model_t *m)
{
    (void) m;
    return s_alarm_count;
}

uint8_t model_sensors_online(const console_model_t *m)
{
    (void) m;
    return 4U;
}

void model_uptime_short(const console_model_t *m, char *buf, size_t size)
{
    (void) m;
    (void) snprintf(buf, size, "1d 02:03");
}

static console_model_t s_m;
static char s_l1[256];
static char s_l2[256];

void setUp(void)
{
    (void) memset(&s_m, 0, sizeof(s_m));
    s_alarm_count = 0U;
    s_m.health_ok = true;
    s_m.alarms_ok = true;
    s_m.sensors_ok = true;
    s_m.health.wifi_rssi_dbm = -52;
    s_m.health.cloud_connected = true;
    s_m.health.modbus_link_online = true;
}

void tearDown(void)
{
}

void test_all_up_shows_green_dots_and_the_signal(void)
{
    header_compose(&s_m, s_l1, sizeof(s_l1), s_l2, sizeof(s_l2));

    TEST_ASSERT_NOT_NULL(strstr(s_l1, "ENVMON GATEWAY"));
    TEST_ASSERT_NOT_NULL(strstr(s_l1, "no alarms"));
    TEST_ASSERT_NOT_NULL(strstr(s_l2, "WiFi"));
    TEST_ASSERT_NOT_NULL(strstr(s_l2, "-52 dBm"));
    TEST_ASSERT_NOT_NULL(strstr(s_l2, theme_sgr(PAL_OK)));
    TEST_ASSERT_NULL(strstr(s_l2, theme_sgr(PAL_ERR)));
}

void test_a_down_link_is_red_and_a_down_modbus_is_amber(void)
{
    s_m.health.wifi_rssi_dbm = 0;
    s_m.health.cloud_connected = false;
    s_m.health.modbus_link_online = false;
    header_compose(&s_m, s_l1, sizeof(s_l1), s_l2, sizeof(s_l2));

    TEST_ASSERT_NOT_NULL(strstr(s_l2, theme_sgr(PAL_ERR)));
    TEST_ASSERT_NOT_NULL(strstr(s_l2, theme_sgr(PAL_WARN)));
    TEST_ASSERT_NULL(strstr(s_l2, theme_sgr(PAL_OK)));
}

void test_unreadable_health_shows_question_marks_not_a_false_green(void)
{
    s_m.health_ok = false;
    header_compose(&s_m, s_l1, sizeof(s_l1), s_l2, sizeof(s_l2));

    TEST_ASSERT_NOT_NULL(strstr(s_l2, "WiFi ?"));
    TEST_ASSERT_NOT_NULL(strstr(s_l2, "MQTT ?"));
    TEST_ASSERT_NOT_NULL(strstr(s_l2, "Modbus ?"));
    TEST_ASSERT_NULL(strstr(s_l2, theme_sgr(PAL_OK)));
}

void test_unreadable_alarms_say_so(void)
{
    s_m.alarms_ok = false;
    header_compose(&s_m, s_l1, sizeof(s_l1), s_l2, sizeof(s_l2));

    TEST_ASSERT_NOT_NULL(strstr(s_l1, "alarms ?"));
}

void test_an_active_alarm_turns_the_first_line_red(void)
{
    s_alarm_count = 2U;
    header_compose(&s_m, s_l1, sizeof(s_l1), s_l2, sizeof(s_l2));

    TEST_ASSERT_NOT_NULL(strstr(s_l1, "ALARM"));
    TEST_ASSERT_NOT_NULL(strstr(s_l1, theme_sgr(PAL_ERR)));
}

void test_the_first_line_carries_firmware_and_uptime(void)
{
    header_compose(&s_m, s_l1, sizeof(s_l1), s_l2, sizeof(s_l2));

    TEST_ASSERT_NOT_NULL(strstr(s_l1, "fw "));
    TEST_ASSERT_NOT_NULL(strstr(s_l1, "up 1d 02:03"));
}
