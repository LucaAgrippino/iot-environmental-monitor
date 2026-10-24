/**
 * @file test_ui_state.c
 * @brief Unity tests for navigation, focus model and the escape-sequence parser
 *        (companion §22.3, §22.4).
 */

#include <string.h>

#include "input.h"
#include "ui_state.h"
#include "unity.h"

static ui_state_t s_ui;

void setUp(void)
{
    ui_init(&s_ui);
    input_reset();
}

void tearDown(void)
{
}

static ui_action_t press(key_kind_t kind, uint8_t ch)
{
    key_event_t k = {.kind = kind, .ch = ch, .rows = 0U, .cols = 0U};

    return ui_handle_key(&s_ui, &k);
}

static void into_panel(uint8_t sel)
{
    s_ui.sel = sel;
    (void) press(KEY_ENTER, 0U);
}

/* Feed a byte string; return the last key produced (KEY_NONE if none). */
static key_event_t feed_all(const uint8_t *bytes, size_t len)
{
    key_event_t last = {.kind = KEY_NONE};
    key_event_t k;

    for (size_t i = 0U; i < len; i++)
    {
        if (input_feed(bytes[i], 100U, &k))
        {
            last = k;
        }
    }
    return last;
}

/* ---------------------------------------------------------------------- */
/* Menu and focus                                                          */
/* ---------------------------------------------------------------------- */

/* TC-GCS-040 */
void test_initial_state_is_first_item_with_menu_focus(void)
{
    TEST_ASSERT_EQUAL_UINT8(0U, s_ui.sel);
    TEST_ASSERT_EQUAL(FOCUS_MENU, s_ui.focus);
    TEST_ASSERT_EQUAL_UINT8(0U, s_ui.channel);
    TEST_ASSERT_EQUAL(SCR_GRAPH, ui_screen(&s_ui));
}

/* TC-GCS-041 */
void test_key_down_moves_the_selection(void)
{
    (void) press(KEY_DOWN, 0U);
    TEST_ASSERT_EQUAL_UINT8(1U, s_ui.sel);
    (void) press(KEY_DOWN, 0U);
    TEST_ASSERT_EQUAL_UINT8(2U, s_ui.sel);
    TEST_ASSERT_EQUAL(SCR_ALARMS, ui_screen(&s_ui));
}

/* TC-GCS-042 */
void test_key_down_at_the_last_item_wraps_to_the_first(void)
{
    s_ui.sel = 6U;
    (void) press(KEY_DOWN, 0U);
    TEST_ASSERT_EQUAL_UINT8(0U, s_ui.sel);
}

/* TC-GCS-043 */
void test_key_up_at_the_first_item_wraps_to_the_last(void)
{
    (void) press(KEY_UP, 0U);
    TEST_ASSERT_EQUAL_UINT8(6U, s_ui.sel);
    TEST_ASSERT_EQUAL(SCR_FIRMWARE, ui_screen(&s_ui));
}

/* TC-GCS-044 */
void test_digit_jumps_straight_to_an_item(void)
{
    (void) press(KEY_DIGIT, 3U);
    TEST_ASSERT_EQUAL_UINT8(2U, s_ui.sel);
    (void) press(KEY_DIGIT, 7U);
    TEST_ASSERT_EQUAL_UINT8(6U, s_ui.sel);
    (void) press(KEY_DIGIT, 1U);
    TEST_ASSERT_EQUAL_UINT8(0U, s_ui.sel);
}

/* TC-GCS-045 */
void test_enter_pushes_focus_into_the_panel(void)
{
    (void) press(KEY_ENTER, 0U);
    TEST_ASSERT_EQUAL(FOCUS_PANEL, s_ui.focus);
}

/* TC-GCS-046 */
void test_tab_pushes_focus_into_the_panel(void)
{
    (void) press(KEY_TAB, 0U);
    TEST_ASSERT_EQUAL(FOCUS_PANEL, s_ui.focus);
}

/* TC-GCS-047 */
void test_escape_returns_focus_to_the_menu_and_keeps_the_screen(void)
{
    into_panel(2U);
    (void) press(KEY_ESCAPE, 0U);

    TEST_ASSERT_EQUAL(FOCUS_MENU, s_ui.focus);
    TEST_ASSERT_EQUAL_UINT8(2U, s_ui.sel);
}

void test_menu_ignores_keys_it_does_not_use(void)
{
    (void) press(KEY_PRINTABLE, 'x');
    (void) press(KEY_BACKSPACE, 0U);
    (void) press(KEY_ESCAPE, 0U);

    TEST_ASSERT_EQUAL_UINT8(0U, s_ui.sel);
    TEST_ASSERT_EQUAL(FOCUS_MENU, s_ui.focus);
}

/* TC-GCS-048 */
void test_graph_panel_cycles_the_channel(void)
{
    into_panel(0U);
    (void) press(KEY_DOWN, 0U);
    TEST_ASSERT_EQUAL_UINT8(1U, s_ui.channel);
    (void) press(KEY_DOWN, 0U);
    TEST_ASSERT_EQUAL_UINT8(2U, s_ui.channel);
    (void) press(KEY_DOWN, 0U);
    TEST_ASSERT_EQUAL_UINT8(0U, s_ui.channel);
    (void) press(KEY_UP, 0U);
    TEST_ASSERT_EQUAL_UINT8(2U, s_ui.channel);
    (void) press(KEY_RIGHT, 0U);
    TEST_ASSERT_EQUAL_UINT8(0U, s_ui.channel);
    (void) press(KEY_LEFT, 0U);
    TEST_ASSERT_EQUAL_UINT8(2U, s_ui.channel);
    TEST_ASSERT_EQUAL_UINT8(0U, s_ui.sel); /* the menu selection did not move */
}

void test_other_panels_only_react_to_escape(void)
{
    into_panel(1U);
    (void) press(KEY_DOWN, 0U);
    (void) press(KEY_PRINTABLE, 'a');
    TEST_ASSERT_EQUAL(FOCUS_PANEL, s_ui.focus);
    TEST_ASSERT_EQUAL_UINT8(1U, s_ui.sel);

    (void) press(KEY_ESCAPE, 0U);
    TEST_ASSERT_EQUAL(FOCUS_MENU, s_ui.focus);
}

/* ---------------------------------------------------------------------- */
/* Console line editing                                                    */
/* ---------------------------------------------------------------------- */

/* TC-GCS-049 */
void test_printable_keys_append_to_the_input_line(void)
{
    into_panel(5U);
    (void) press(KEY_PRINTABLE, 'h');
    (void) press(KEY_PRINTABLE, 'i');

    TEST_ASSERT_EQUAL_STRING("hi", s_ui.input);
    TEST_ASSERT_EQUAL_UINT8(2U, s_ui.input_len);
}

/* TC-GCS-050 */
void test_backspace_deletes_the_last_character_and_stops_at_empty(void)
{
    into_panel(5U);
    (void) press(KEY_PRINTABLE, 'a');
    (void) press(KEY_PRINTABLE, 'b');
    (void) press(KEY_BACKSPACE, 0U);
    TEST_ASSERT_EQUAL_STRING("a", s_ui.input);

    (void) press(KEY_BACKSPACE, 0U);
    (void) press(KEY_BACKSPACE, 0U);
    TEST_ASSERT_EQUAL_STRING("", s_ui.input);
    TEST_ASSERT_EQUAL_UINT8(0U, s_ui.input_len);
}

/* TC-GCS-051 */
void test_enter_on_the_console_asks_the_caller_to_run_the_line(void)
{
    into_panel(5U);
    (void) press(KEY_PRINTABLE, 'x');

    TEST_ASSERT_EQUAL(UI_ACT_RUN_LINE, press(KEY_ENTER, 0U));
    TEST_ASSERT_EQUAL_STRING("x", s_ui.input); /* the caller clears it after running */
    ui_clear_input(&s_ui);
    TEST_ASSERT_EQUAL_UINT8(0U, s_ui.input_len);
}

/* TC-GCS-052 */
void test_digits_are_literal_text_on_the_console_not_menu_jumps(void)
{
    into_panel(5U);
    (void) press(KEY_DIGIT, 3U);
    (void) press(KEY_PRINTABLE, '9');

    TEST_ASSERT_EQUAL_STRING("39", s_ui.input);
    TEST_ASSERT_EQUAL_UINT8(5U, s_ui.sel);
}

void test_console_escape_returns_to_the_menu_and_keeps_the_input(void)
{
    into_panel(5U);
    (void) press(KEY_PRINTABLE, 'q');
    (void) press(KEY_ESCAPE, 0U);

    TEST_ASSERT_EQUAL(FOCUS_MENU, s_ui.focus);
    TEST_ASSERT_EQUAL_STRING("q", s_ui.input);
}

/* TC-GCS-119 */
void test_a_full_input_line_overflows_and_is_cleared(void)
{
    into_panel(5U);
    for (uint32_t i = 0U; i < (UI_INPUT_MAX - 1U); i++)
    {
        TEST_ASSERT_EQUAL(UI_ACT_NONE, press(KEY_PRINTABLE, 'a'));
    }
    TEST_ASSERT_EQUAL_UINT8(UI_INPUT_MAX - 1U, s_ui.input_len);

    TEST_ASSERT_EQUAL(UI_ACT_LINE_OVERFLOW, press(KEY_PRINTABLE, 'b'));
    TEST_ASSERT_EQUAL_UINT8(0U, s_ui.input_len);
    TEST_ASSERT_EQUAL_STRING("", s_ui.input);
}

void test_confirm_prompt_takes_over_the_keyboard(void)
{
    into_panel(5U);
    s_ui.confirm_active = true;

    TEST_ASSERT_EQUAL(UI_ACT_CONFIRM_YES, press(KEY_PRINTABLE, 'y'));
    TEST_ASSERT_EQUAL(UI_ACT_CONFIRM_YES, press(KEY_PRINTABLE, 'Y'));
    TEST_ASSERT_EQUAL(UI_ACT_CONFIRM_NO, press(KEY_PRINTABLE, 'n'));
    TEST_ASSERT_EQUAL(UI_ACT_CONFIRM_NO, press(KEY_ENTER, 0U));
    TEST_ASSERT_EQUAL(UI_ACT_CONFIRM_NO, press(KEY_ESCAPE, 0U));
    TEST_ASSERT_EQUAL(UI_ACT_CONFIRM_NO, press(KEY_DIGIT, 1U));
    TEST_ASSERT_EQUAL_UINT8(0U, s_ui.input_len); /* nothing leaked into the input line */
}

/* ---------------------------------------------------------------------- */
/* Escape-sequence parser                                                  */
/* ---------------------------------------------------------------------- */

/* TC-GCS-060..063 */
void test_csi_arrows_decode(void)
{
    static const uint8_t up[] = {0x1B, 0x5B, 0x41};
    static const uint8_t down[] = {0x1B, 0x5B, 0x42};
    static const uint8_t right[] = {0x1B, 0x5B, 0x43};
    static const uint8_t left[] = {0x1B, 0x5B, 0x44};

    TEST_ASSERT_EQUAL(KEY_UP, feed_all(up, sizeof(up)).kind);
    TEST_ASSERT_EQUAL(KEY_DOWN, feed_all(down, sizeof(down)).kind);
    TEST_ASSERT_EQUAL(KEY_RIGHT, feed_all(right, sizeof(right)).kind);
    TEST_ASSERT_EQUAL(KEY_LEFT, feed_all(left, sizeof(left)).kind);
}

/* TC-GCS-064 */
void test_a_lone_escape_is_reported_after_its_window(void)
{
    key_event_t k;

    TEST_ASSERT_FALSE(input_feed(0x1B, 1000U, &k));
    TEST_ASSERT_TRUE(input_esc_pending());
    TEST_ASSERT_FALSE(input_poll_timeout(1000U + INPUT_ESC_TIMEOUT_MS - 1U, &k));
    TEST_ASSERT_TRUE(input_poll_timeout(1000U + INPUT_ESC_TIMEOUT_MS, &k));
    TEST_ASSERT_EQUAL(KEY_ESCAPE, k.kind);
    TEST_ASSERT_FALSE(input_esc_pending());
    TEST_ASSERT_FALSE(input_poll_timeout(5000U, &k)); /* reported once */
}

/* TC-GCS-065 */
void test_application_cursor_mode_arrows_decode(void)
{
    static const uint8_t up[] = {0x1B, 0x4F, 0x41};
    static const uint8_t left[] = {0x1B, 0x4F, 0x44};

    TEST_ASSERT_EQUAL(KEY_UP, feed_all(up, sizeof(up)).kind);
    TEST_ASSERT_EQUAL(KEY_LEFT, feed_all(left, sizeof(left)).kind);
}

/* TC-GCS-066 */
void test_cr_and_lf_are_both_enter(void)
{
    key_event_t k;

    TEST_ASSERT_TRUE(input_feed(0x0D, 0U, &k));
    TEST_ASSERT_EQUAL(KEY_ENTER, k.kind);
    TEST_ASSERT_TRUE(input_feed(0x0A, 0U, &k));
    TEST_ASSERT_EQUAL(KEY_ENTER, k.kind);
}

/* TC-GCS-067 */
void test_del_and_bs_are_both_backspace(void)
{
    key_event_t k;

    TEST_ASSERT_TRUE(input_feed(0x7F, 0U, &k));
    TEST_ASSERT_EQUAL(KEY_BACKSPACE, k.kind);
    TEST_ASSERT_TRUE(input_feed(0x08, 0U, &k));
    TEST_ASSERT_EQUAL(KEY_BACKSPACE, k.kind);
}

/* TC-GCS-068 */
void test_tab_decodes(void)
{
    key_event_t k;

    TEST_ASSERT_TRUE(input_feed(0x09, 0U, &k));
    TEST_ASSERT_EQUAL(KEY_TAB, k.kind);
}

/* TC-GCS-069 */
void test_printable_and_digit_bytes_decode(void)
{
    key_event_t k;

    TEST_ASSERT_TRUE(input_feed('a', 0U, &k));
    TEST_ASSERT_EQUAL(KEY_PRINTABLE, k.kind);
    TEST_ASSERT_EQUAL_UINT8('a', k.ch);
    TEST_ASSERT_TRUE(input_feed('4', 0U, &k));
    TEST_ASSERT_EQUAL(KEY_DIGIT, k.kind);
    TEST_ASSERT_EQUAL_UINT8(4U, k.ch);
    TEST_ASSERT_TRUE(input_feed('0', 0U, &k)); /* only 1..7 are menu digits */
    TEST_ASSERT_EQUAL(KEY_PRINTABLE, k.kind);
    TEST_ASSERT_TRUE(input_feed('~', 0U, &k));
    TEST_ASSERT_EQUAL(KEY_PRINTABLE, k.kind);
}

void test_page_keys_decode(void)
{
    static const uint8_t up[] = {0x1B, '[', '5', '~'};
    static const uint8_t down[] = {0x1B, '[', '6', '~'};
    static const uint8_t home[] = {0x1B, '[', '1', '~'};

    TEST_ASSERT_EQUAL(KEY_PGUP, feed_all(up, sizeof(up)).kind);
    TEST_ASSERT_EQUAL(KEY_PGDN, feed_all(down, sizeof(down)).kind);
    TEST_ASSERT_EQUAL(KEY_NONE, feed_all(home, sizeof(home)).kind);
}

void test_page_keys_scroll_the_console_view_in_steps_and_stop_at_live(void)
{
    into_panel(5U);
    (void) press(KEY_PGUP, 0U);
    TEST_ASSERT_EQUAL_UINT16(UI_SCROLL_STEP, s_ui.scroll);
    (void) press(KEY_PGUP, 0U);
    TEST_ASSERT_EQUAL_UINT16(2U * UI_SCROLL_STEP, s_ui.scroll);
    (void) press(KEY_PGDN, 0U);
    TEST_ASSERT_EQUAL_UINT16(UI_SCROLL_STEP, s_ui.scroll);
    (void) press(KEY_PGDN, 0U);
    (void) press(KEY_PGDN, 0U);
    TEST_ASSERT_EQUAL_UINT16(0U, s_ui.scroll);
}

void test_page_keys_do_nothing_outside_the_console(void)
{
    into_panel(0U);
    (void) press(KEY_PGUP, 0U);
    TEST_ASSERT_EQUAL_UINT16(0U, s_ui.scroll);
}

void test_ctrl_l_decodes_as_a_redraw_request(void)
{
    key_event_t k;

    TEST_ASSERT_TRUE(input_feed(0x0C, 0U, &k));
    TEST_ASSERT_EQUAL(KEY_REDRAW, k.kind);
}

void test_control_and_high_bytes_are_ignored(void)
{
    key_event_t k;

    TEST_ASSERT_FALSE(input_feed(0x03, 0U, &k)); /* Ctrl-C */
    TEST_ASSERT_FALSE(input_feed(0x00, 0U, &k));
    TEST_ASSERT_FALSE(input_feed(0xC3, 0U, &k)); /* UTF-8 lead byte */
}

void test_escape_followed_by_another_key_reports_escape_at_once(void)
{
    key_event_t k;

    TEST_ASSERT_FALSE(input_feed(0x1B, 0U, &k));
    TEST_ASSERT_TRUE(input_feed('x', 1U, &k)); /* not '[' or 'O': the ESC was the Escape key */
    TEST_ASSERT_EQUAL(KEY_ESCAPE, k.kind);
    TEST_ASSERT_FALSE(input_esc_pending());

    TEST_ASSERT_TRUE(input_take_pending(&k)); /* the byte that followed is not lost */
    TEST_ASSERT_EQUAL(KEY_PRINTABLE, k.kind);
    TEST_ASSERT_EQUAL_UINT8('x', k.ch);
    TEST_ASSERT_FALSE(input_take_pending(&k)); /* handed over once */
}

void test_a_digit_right_after_escape_still_jumps_the_menu(void)
{
    key_event_t k;

    (void) input_feed(0x1B, 0U, &k);
    TEST_ASSERT_TRUE(input_feed('3', 1U, &k));
    TEST_ASSERT_EQUAL(KEY_ESCAPE, k.kind);
    TEST_ASSERT_TRUE(input_take_pending(&k));
    TEST_ASSERT_EQUAL(KEY_DIGIT, k.kind);
    TEST_ASSERT_EQUAL_UINT8(3U, k.ch);
}

void test_a_control_byte_after_escape_yields_no_second_key(void)
{
    key_event_t k;

    (void) input_feed(0x1B, 0U, &k);
    TEST_ASSERT_TRUE(input_feed(0x03, 1U, &k)); /* Ctrl-C after ESC */
    TEST_ASSERT_FALSE(input_take_pending(&k));
}

void test_two_escapes_in_a_row_report_one_and_wait_on_the_second(void)
{
    key_event_t k;

    TEST_ASSERT_FALSE(input_feed(0x1B, 0U, &k));
    TEST_ASSERT_TRUE(input_feed(0x1B, 1U, &k));
    TEST_ASSERT_EQUAL(KEY_ESCAPE, k.kind);
    TEST_ASSERT_TRUE(input_esc_pending());
}

void test_unused_csi_sequences_are_swallowed(void)
{
    static const uint8_t home[] = {0x1B, '[', '1', '~'};
    static const uint8_t f5[] = {0x1B, '[', '1', '5', '~'};

    TEST_ASSERT_EQUAL(KEY_NONE, feed_all(home, sizeof(home)).kind);
    TEST_ASSERT_EQUAL(KEY_NONE, feed_all(f5, sizeof(f5)).kind);

    key_event_t k;

    TEST_ASSERT_TRUE(input_feed('a', 0U, &k)); /* the parser is back in sync */
    TEST_ASSERT_EQUAL(KEY_PRINTABLE, k.kind);
}

void test_a_runaway_csi_sequence_is_abandoned(void)
{
    key_event_t k;

    (void) input_feed(0x1B, 0U, &k);
    (void) input_feed('[', 0U, &k);
    for (uint32_t i = 0U; i < 40U; i++)
    {
        (void) input_feed('1', 0U, &k);
    }
    TEST_ASSERT_TRUE(input_feed('b', 0U, &k));
    TEST_ASSERT_EQUAL(KEY_PRINTABLE, k.kind);
}

void test_terminal_size_report_is_decoded(void)
{
    static const uint8_t report[] = {0x1B, '[', '8', ';', '4', '2', ';', '1', '2', '0', 't'};
    const key_event_t k = feed_all(report, sizeof(report));

    TEST_ASSERT_EQUAL(KEY_SIZE_REPORT, k.kind);
    TEST_ASSERT_EQUAL_UINT16(42U, k.rows);
    TEST_ASSERT_EQUAL_UINT16(120U, k.cols);
}

void test_malformed_size_reports_are_ignored(void)
{
    static const uint8_t wrong_kind[] = {0x1B, '[', '9', ';', '4', ';', '1', 't'};
    static const uint8_t too_few[] = {0x1B, '[', '8', ';', '4', 't'};
    static const uint8_t too_big[] = {0x1B, '[', '8', ';', '4', ';', '1', '2', '3', '4', '5', 't'};

    TEST_ASSERT_EQUAL(KEY_NONE, feed_all(wrong_kind, sizeof(wrong_kind)).kind);
    TEST_ASSERT_EQUAL(KEY_NONE, feed_all(too_few, sizeof(too_few)).kind);
    TEST_ASSERT_EQUAL(KEY_NONE, feed_all(too_big, sizeof(too_big)).kind);
}
