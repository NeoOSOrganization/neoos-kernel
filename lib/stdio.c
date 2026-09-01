#include "stdio.h"
#include "unistd.h"
#include "fcntl.h"
#include <stdint.h>
#include <stdarg.h>

#define PRINTF_BUFFER_SIZE 512

// libneoos's printf targets /dev/kmsg (serial only), NOT stdout. Its
// only users are NeoOS's own system/test programs (init, the terminal,
// the *test suites) whose output is diagnostic chatter -- it must not
// land on the framebuffer console. Opened once, lazily; falls back to
// fd 1 if /dev/kmsg is somehow unavailable.
static int kmsg_fd = -2;   // -2 = not tried, -1 = unavailable, >=0 = open

static void printf_out(const char *buf, uint64_t n) {
    if (kmsg_fd == -2) {
        kmsg_fd = open("/dev/kmsg", O_WRONLY);
    }
    write(kmsg_fd >= 0 ? kmsg_fd : STDOUT_FILENO, buf, n);
}

static void append_char(char *buf, uint64_t *pos, char c) {
    if (*pos < PRINTF_BUFFER_SIZE - 1) {
        buf[(*pos)++] = c;
    }
}

static void append_string(char *buf, uint64_t *pos, const char *s) {
    while (*s) {
        append_char(buf, pos, *s);
        s++;
    }
}

static void append_uint(char *buf, uint64_t *pos, uint64_t value, int base, int uppercase) {
    static const char *digits_lower = "0123456789abcdef";
    static const char *digits_upper = "0123456789ABCDEF";
    const char *digits = uppercase ? digits_upper : digits_lower;
    char tmp[32];
    int i = 0;
    if (value == 0) {
        tmp[i++] = '0';
    }
    while (value > 0) {
        tmp[i++] = digits[value % (uint64_t)base];
        value /= (uint64_t)base;
    }
    while (i > 0) {
        append_char(buf, pos, tmp[--i]);
    }
}

static void append_int(char *buf, uint64_t *pos, int64_t value) {
    if (value < 0) {
        append_char(buf, pos, '-');
        append_uint(buf, pos, (uint64_t)(-value), 10, 0);
    } else {
        append_uint(buf, pos, (uint64_t)value, 10, 0);
    }
}

int printf(const char *fmt, ...) {
    char buf[PRINTF_BUFFER_SIZE];
    uint64_t pos = 0;

    va_list args;
    va_start(args, fmt);

    for (uint64_t i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            append_char(buf, &pos, fmt[i]);
            continue;
        }

        i++;
        if (fmt[i] == '\0') {
            append_char(buf, &pos, '%');
            break;
        }

        switch (fmt[i]) {
            case 's': {
                const char *s = va_arg(args, const char *);
                append_string(buf, &pos, s);
                break;
            }
            case 'd': {
                int value = va_arg(args, int);
                append_int(buf, &pos, value);
                break;
            }
            case 'u': {
                unsigned int value = va_arg(args, unsigned int);
                append_uint(buf, &pos, value, 10, 0);
                break;
            }
            case 'x': {
                unsigned int value = va_arg(args, unsigned int);
                append_uint(buf, &pos, value, 16, 0);
                break;
            }
            case 'c': {
                int value = va_arg(args, int);
                append_char(buf, &pos, (char)value);
                break;
            }
            case '%':
                append_char(buf, &pos, '%');
                break;
            default:
                append_char(buf, &pos, '%');
                append_char(buf, &pos, fmt[i]);
                break;
        }
    }

    va_end(args);

    printf_out(buf, pos);
    return (int)pos;
}
