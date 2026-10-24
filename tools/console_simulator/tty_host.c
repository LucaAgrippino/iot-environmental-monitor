/**
 * @file tty_host.c
 * @brief Raw terminal I/O for the host simulator (kept apart: <termios.h> clashes with CMSIS
 * names).
 */

#include <stddef.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

static struct termios s_old;

void tty_raw_on(void)
{
    struct termios raw;

    tcgetattr(STDIN_FILENO, &s_old);
    raw = s_old;
    raw.c_lflag &= ~(tcflag_t) (ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; /* 100 ms */
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void tty_raw_off(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &s_old);
}

long tty_read(char *buf, unsigned long n)
{
    return (long) read(STDIN_FILENO, buf, n);
}
