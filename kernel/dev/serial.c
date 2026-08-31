#include "dev/serial.h"
#include "arch/io.h"
#include "sync/lock.h"

#define COM1 0x3F8

// Guards whole messages, not single characters. Uses the RANK-FREE
// raw acquire: serial output starts on the first line of kmain, long
// before cpu_local_init() installs a GS base, and the rank checker
// reads per-CPU state through GS. Without it a timer
// interrupt landing mid-print lets another thread's output interleave
// into the middle of a line -- observed on ONE cpu as
// "[process] task exited, pid=0x000000000child running, exiting...".
// The serial log is this project's only debugging channel, so a line
// that cannot be trusted to be whole is worse than a slow print.
static struct spinlock serial_lock;

void serial_init(void) {
    outb(COM1 + 1, 0x00); // disable interrupts
    outb(COM1 + 3, 0x80); // enable DLAB
    outb(COM1 + 0, 0x03); // divisor low byte: 38400 baud
    outb(COM1 + 1, 0x00); // divisor high byte
    outb(COM1 + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7); // enable FIFO, clear it, 14-byte threshold
    outb(COM1 + 4, 0x0B); // IRQs disabled, RTS/DSR set
    spin_init(&serial_lock, LOCK_RANK_SERIAL, "serial");
}

static int transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

// Unlocked single-character write. Callers emitting a whole message
// should use serial_write_string/_n so the message cannot be split.
void serial_putc(char c) {
    while (!transmit_empty()) { }
    outb(COM1, (uint8_t)c);
}

static void put_cooked(char c) {
    if (c == '\n') { serial_putc('\r'); }
    serial_putc(c);
}

void serial_write_string(const char *str) {
    uint64_t f = spin_lock_raw(&serial_lock);
    for (int i = 0; str[i] != '\0'; i++) { put_cooked(str[i]); }
    spin_unlock_raw(&serial_lock, f);
}

void serial_write_string_n(const char *str, uint64_t len) {
    uint64_t f = spin_lock_raw(&serial_lock);
    for (uint64_t i = 0; i < len; i++) { put_cooked(str[i]); }
    spin_unlock_raw(&serial_lock, f);
}

void serial_write_hex64(uint64_t value) {
    static const char hex_digits[] = "0123456789abcdef";
    uint64_t f = spin_lock_raw(&serial_lock);
    put_cooked('0'); put_cooked('x');
    for (int shift = 60; shift >= 0; shift -= 4) {
        serial_putc(hex_digits[(value >> shift) & 0xF]);
    }
    spin_unlock_raw(&serial_lock, f);
}
