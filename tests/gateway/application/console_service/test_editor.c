/**
 * @file test_editor.c
 * @brief Unity tests for the console line editor.
 */

#include <string.h>

#include "editor.h"
#include "unity.h"

static editor_t s_ed;

void setUp(void)
{
    editor_init(&s_ed);
}

void tearDown(void)
{
}

static editor_action_t key(key_kind_t kind, uint8_t ch, bool confirm)
{
    const key_event_t k = {.kind = kind, .ch = ch};

    return editor_key(&s_ed, &k, confirm);
}

static void type(const char *text)
{
    for (; *text != '\0'; text++)
    {
        (void) key(KEY_PRINTABLE, (uint8_t) *text, false);
    }
}

void test_printable_keys_build_the_line(void)
{
    TEST_ASSERT_EQUAL(ED_CHANGED, key(KEY_PRINTABLE, 's', false));
    TEST_ASSERT_EQUAL(ED_CHANGED, key(KEY_PRINTABLE, 'e', false));
    TEST_ASSERT_EQUAL_STRING("se", s_ed.line);
}

void test_digits_are_typed_like_any_other_character(void)
{
    type("set t.hi 3");
    (void) key(KEY_DIGIT, 5U, false);
    TEST_ASSERT_EQUAL_STRING("set t.hi 35", s_ed.line);
}

void test_backspace_removes_the_last_character_and_ignores_an_empty_line(void)
{
    TEST_ASSERT_EQUAL(ED_NONE, key(KEY_BACKSPACE, 0U, false));
    type("ab");
    TEST_ASSERT_EQUAL(ED_CHANGED, key(KEY_BACKSPACE, 0U, false));
    TEST_ASSERT_EQUAL_STRING("a", s_ed.line);
}

void test_escape_clears_a_typed_line_only(void)
{
    TEST_ASSERT_EQUAL(ED_NONE, key(KEY_ESCAPE, 0U, false));
    type("abc");
    TEST_ASSERT_EQUAL(ED_CHANGED, key(KEY_ESCAPE, 0U, false));
    TEST_ASSERT_EQUAL_STRING("", s_ed.line);
}

void test_enter_asks_the_caller_to_run_the_line(void)
{
    type("status");
    TEST_ASSERT_EQUAL(ED_RUN_LINE, key(KEY_ENTER, 0U, false));
    TEST_ASSERT_EQUAL_STRING("status", s_ed.line); /* kept until the caller clears it */
    editor_clear(&s_ed);
    TEST_ASSERT_EQUAL_STRING("", s_ed.line);
}

void test_up_recalls_the_last_line_that_was_run(void)
{
    TEST_ASSERT_EQUAL(ED_NONE, key(KEY_UP, 0U, false));
    type("alarms");
    (void) key(KEY_ENTER, 0U, false);
    editor_clear(&s_ed);
    TEST_ASSERT_EQUAL(ED_CHANGED, key(KEY_UP, 0U, false));
    TEST_ASSERT_EQUAL_STRING("alarms", s_ed.line);
    TEST_ASSERT_EQUAL_UINT8(6U, s_ed.len);
    (void) key(KEY_PRINTABLE, '!', false);
    TEST_ASSERT_EQUAL_STRING("alarms!", s_ed.line);
}

void test_an_empty_enter_does_not_replace_the_history(void)
{
    type("net");
    (void) key(KEY_ENTER, 0U, false);
    editor_clear(&s_ed);
    (void) key(KEY_ENTER, 0U, false);
    (void) key(KEY_UP, 0U, false);
    TEST_ASSERT_EQUAL_STRING("net", s_ed.line);
}

void test_a_full_line_is_cleared_and_reported(void)
{
    editor_action_t last = ED_NONE;

    for (uint32_t i = 0U; i < EDITOR_LINE_MAX; i++)
    {
        last = key(KEY_PRINTABLE, 'x', false);
    }
    TEST_ASSERT_EQUAL(ED_LINE_OVERFLOW, last);
    TEST_ASSERT_EQUAL_STRING("", s_ed.line);
    TEST_ASSERT_EQUAL_UINT8(0U, s_ed.len);
}

void test_ctrl_l_asks_for_a_repaint(void)
{
    TEST_ASSERT_EQUAL(ED_REDRAW, key(KEY_REDRAW, 0U, false));
}

void test_confirm_prompt_takes_y_as_yes_and_anything_else_as_no(void)
{
    TEST_ASSERT_EQUAL(ED_CONFIRM_YES, key(KEY_PRINTABLE, 'y', true));
    TEST_ASSERT_EQUAL(ED_CONFIRM_YES, key(KEY_PRINTABLE, 'Y', true));
    TEST_ASSERT_EQUAL(ED_CONFIRM_NO, key(KEY_PRINTABLE, 'n', true));
    TEST_ASSERT_EQUAL(ED_CONFIRM_NO, key(KEY_ENTER, 0U, true));
    TEST_ASSERT_EQUAL(ED_CONFIRM_NO, key(KEY_ESCAPE, 0U, true));
    TEST_ASSERT_EQUAL(ED_CONFIRM_NO, key(KEY_DIGIT, 3U, true));
}

void test_confirm_prompt_ignores_arrows_and_never_edits_the_line(void)
{
    type("abc");
    TEST_ASSERT_EQUAL(ED_NONE, key(KEY_UP, 0U, true));
    TEST_ASSERT_EQUAL(ED_NONE, key(KEY_LEFT, 0U, true));
    TEST_ASSERT_EQUAL(ED_CONFIRM_NO, key(KEY_PRINTABLE, 'q', true));
    TEST_ASSERT_EQUAL_STRING("abc", s_ed.line);
}
