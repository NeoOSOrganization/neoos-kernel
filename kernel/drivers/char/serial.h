#ifndef NEOOS_SERIAL_H
#define NEOOS_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putc(char c);
void serial_write_string(const char *str);
void serial_write_string_n(const char *str, uint64_t len);
// Like serial_write_string_n but writes the bytes verbatim -- no CR is
// inserted before a LF. For a terminal in raw mode (OPOST off), whose
// bytes must reach the wire unmodified. Still takes serial_lock, so it
// cannot interleave with other serial output.
void serial_write_raw_n(const char *str, uint64_t len);
void serial_write_hex64(uint64_t value);

#endif
