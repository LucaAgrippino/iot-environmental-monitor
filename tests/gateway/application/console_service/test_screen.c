/**
 * @file test_screen.c
 * @brief Unity tests for the three-panel screen writer: the exact escape sequences it sends.
 *
 * The layout at the default 36 x 100 size: header rows 1-2, rule 3, log 4..24, divider 25,
 * results 26..35, prompt 36.
 */

#include <string.h>

#include "screen.h"
#include "theme.h"
#include "unity.h"

static char s_out[4096];
static size_t s_out_len;
static int s_sends;
static int s_depth;
static int s_max_depth;
static bool s_sent_while_unlocked;

static bool fake_send(const uint8_t *data, size_t len)
{
    if (s_depth == 0)
    {
        s_sent_while_unlocked = true;
    }
    (void) memcpy(&s_out[s_out_len], data, len);
    s_out_len += len;
    s_out[s_out_len] = '\0';
    s_sends++;
    return true;
}

static void fake_lock(void)
{
    s_depth++;
    if (s_depth > s_max_depth)
    {
        s_max_depth = s_depth;
    }
}

static void fake_unlock(void)
{
    s_depth--;
}

static void forget(void)
{
    s_out_len = 0U;
    s_out[0] = '\0';
    s_sends = 0;
}

void setUp(void)
{
    const screen_port_t port = {.send = fake_send, .lock = fake_lock, .unlock = fake_unlock};

    s_depth = 0;
    s_max_depth = 0;
    s_sent_while_unlocked = false;
    screen_init(&port);
    forget();
}

void tearDown(void)
{
}

void test_open_clears_the_screen_and_draws_the_rule_and_divider(void)
{
    screen_open();

    TEST_ASSERT_TRUE(screen_is_open());
    TEST_ASSERT_NOT_NULL(strstr(s_out, "\x1b[2J"));
    TEST_ASSERT_NOT_NULL(strstr(s_out, "\x1b[3;1H"));  /* header rule */
    TEST_ASSERT_NOT_NULL(strstr(s_out, "\x1b[25;1H")); /* divider */
    TEST_ASSERT_NOT_NULL(strstr(s_out, "console"));
    TEST_ASSERT_EQUAL_INT(1, s_sends); /* one write: the frame cannot be split */
}

void test_log_line_scrolls_only_the_centre_region_then_returns_to_the_prompt(void)
{
    screen_open();
    forget();
    screen_log("[INFO][12:00:00][wifi] joined\r\n", 31U);

    TEST_ASSERT_NOT_NULL(strstr(s_out, "\x1b[4;24r"));     /* region = log rows */
    TEST_ASSERT_NOT_NULL(strstr(s_out, "\x1b[24;1H\n\r")); /* last log row, then scroll */
    TEST_ASSERT_NOT_NULL(strstr(s_out, "[INFO][12:00:00][wifi] joined"));
    TEST_ASSERT_NULL(strstr(s_out, "joined\r")); /* trailing CR / LF dropped */
    TEST_ASSERT_TRUE(strstr(s_out, "joined") < strstr(s_out, "\x1b[36;1H")); /* cursor back last */
}

void test_result_line_scrolls_the_south_region_in_its_colour(void)
{
    screen_open();
    forget();
    screen_result(PAL_ERR, "[ERR] nope");

    TEST_ASSERT_NOT_NULL(strstr(s_out, "\x1b[26;35r"));
    TEST_ASSERT_NOT_NULL(strstr(s_out, "\x1b[35;1H\n\r"));
    TEST_ASSERT_NOT_NULL(strstr(s_out, theme_sgr(PAL_ERR)));
    TEST_ASSERT_NOT_NULL(strstr(s_out, "[ERR] nope"));
}

void test_prompt_redraws_its_row_and_later_writes_return_the_cursor_to_its_end(void)
{
    screen_open();
    screen_prompt("envmon> ", "status");
    TEST_ASSERT_NOT_NULL(strstr(s_out + (s_out_len - 40U), "status"));

    forget();
    screen_log("x", 1U);
    /* 8 label columns + 6 typed + 1 = the cursor sits in column 15 */
    TEST_ASSERT_NOT_NULL(strstr(s_out, "\x1b[36;15H"));
}

void test_long_prompt_text_shows_its_tail(void)
{
    char text[121];

    memset(text, 'a', sizeof(text));
    text[119] = 'Z';
    text[120] = '\0';
    screen_open();
    forget();
    screen_prompt("envmon> ", text);

    TEST_ASSERT_NOT_NULL(strstr(s_out, "aZ"));
    TEST_ASSERT_TRUE(s_out_len < 140U); /* cut to the row, not 120 characters */
}

void test_log_line_is_clipped_to_the_width_without_counting_colour_codes(void)
{
    char line[400];
    size_t n = 0U;

    n += (size_t) snprintf(line, sizeof(line), "\x1b[31mERR\x1b[0m ");
    memset(&line[n], 'k', 150U);
    n += 150U;
    screen_open();
    forget();
    screen_log(line, n);

    TEST_ASSERT_NOT_NULL(strstr(s_out, "\x1b[31mERR\x1b[0m ")); /* colour kept whole */
    const char *first = strstr(s_out, "ERR\x1b[0m ") + 8;
    size_t ks = 0U;

    while (first[ks] == 'k')
    {
        ks++;
    }
    TEST_ASSERT_EQUAL_UINT32(99U - 4U, ks); /* 99 visible columns minus "ERR " */
}

void test_utf8_characters_count_as_one_column_and_are_never_cut(void)
{
    char line[400];
    size_t n = 0U;

    for (int i = 0; i < 120; i++)
    {
        line[n++] = (char) 0xE2; /* U+25CF, three bytes, one column */
        line[n++] = (char) 0x97;
        line[n++] = (char) 0x8F;
    }
    screen_open();
    forget();
    screen_log(line, n);

    const char *p = strstr(s_out, "\xE2\x97\x8F");
    size_t dots = 0U;

    while (strncmp(p, "\xE2\x97\x8F", 3U) == 0)
    {
        dots++;
        p += 3;
    }
    TEST_ASSERT_EQUAL_UINT32(99U, dots);
}

void test_control_bytes_and_foreign_escapes_never_reach_the_terminal(void)
{
    screen_open();
    forget();
    screen_log("a\x07\x1b[2Jb\tc", 10U);

    TEST_ASSERT_NOT_NULL(strstr(s_out, "abc"));
    TEST_ASSERT_NULL(strstr(s_out, "\x1b[2J"));
    TEST_ASSERT_NULL(strchr(s_out, '\x07'));
}

void test_header_rewrites_rows_one_and_two_in_place(void)
{
    screen_open();
    forget();
    screen_header(" line one", " line two");

    TEST_ASSERT_NOT_NULL(strstr(s_out, "\x1b[1;1H\x1b[2K line one"));
    TEST_ASSERT_NOT_NULL(strstr(s_out, "\x1b[2;1H\x1b[2K line two"));
    TEST_ASSERT_NULL(strstr(s_out, "\x1b[4;24r")); /* no scroll region involved */
}

void test_clear_empties_only_the_south_rows(void)
{
    screen_open();
    forget();
    screen_results_clear();

    TEST_ASSERT_NOT_NULL(strstr(s_out, "\x1b[26;1H\x1b[2K"));
    TEST_ASSERT_NOT_NULL(strstr(s_out, "\x1b[35;1H\x1b[2K"));
    TEST_ASSERT_NULL(strstr(s_out, "\x1b[25;1H"));
    TEST_ASSERT_NULL(strstr(s_out, "\x1b[36;1H\x1b[2K"));
}

void test_before_open_a_log_line_is_written_plain(void)
{
    screen_log("boot\r\n", 6U);

    TEST_ASSERT_EQUAL_STRING("boot\r\n", s_out);
}

void test_every_write_happens_inside_the_lock(void)
{
    screen_open();
    screen_header("a", "b");
    screen_log("x", 1U);
    screen_result(PAL_INK, "y");
    screen_prompt("p> ", "z");
    screen_results_clear();
    screen_close();

    TEST_ASSERT_FALSE(s_sent_while_unlocked);
    TEST_ASSERT_EQUAL_INT(0, s_depth); /* balanced */
    TEST_ASSERT_EQUAL_INT(1, s_max_depth);
}

void test_size_is_clamped_and_moves_the_layout(void)
{
    uint16_t r;
    uint16_t c;

    screen_set_size(5U, 5000U);
    screen_size(&r, &c);
    TEST_ASSERT_EQUAL_UINT16(SCREEN_ROWS_MIN, r);
    TEST_ASSERT_EQUAL_UINT16(SCREEN_COLS_MAX, c);

    screen_set_size(50U, 120U);
    screen_open();
    forget();
    screen_log("x", 1U);
    TEST_ASSERT_NOT_NULL(strstr(s_out, "\x1b[4;38r")); /* divider at 50-10-1 = 39 */
}

void test_close_gives_the_terminal_back(void)
{
    screen_open();
    forget();
    screen_close();

    TEST_ASSERT_FALSE(screen_is_open());
    TEST_ASSERT_NOT_NULL(strstr(s_out, "\x1b[r"));
}
