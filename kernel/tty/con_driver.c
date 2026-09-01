#include "tty/con_driver.h"
#include "drivers/char/serial.h"

#define CON_DRIVER_MAX 4

static struct con_driver *registry[CON_DRIVER_MAX];
static int                registry_n;
static struct con_driver *active;
static int con_cols, con_rows;

extern struct con_driver fbcon_drv;
extern struct con_driver vgacon_drv;
extern struct con_driver dummycon_drv;

void con_driver_register(struct con_driver *d) {
    if (registry_n < CON_DRIVER_MAX) { registry[registry_n++] = d; }
}

void con_driver_register_builtin(void) {
    con_driver_register(&fbcon_drv);
    con_driver_register(&vgacon_drv);
    con_driver_register(&dummycon_drv);
}

void con_driver_select(void) {
    struct con_driver *best = 0;
    for (int i = 0; i < registry_n; i++) {
        struct con_driver *d = registry[i];
        if (d->probe && d->probe()) {
            if (!best || d->priority > best->priority) { best = d; }
        }
    }
    active = best;
    if (active) {
        active->init(&con_cols, &con_rows);
        serial_write_string("[con] ");
        serial_write_string(active->name);
        serial_write_string(" selected, ");
        serial_write_hex64((uint64_t)con_cols);
        serial_write_string("x");
        serial_write_hex64((uint64_t)con_rows);
        serial_write_string("\n");
    } else {
        serial_write_string("[con] no console driver\n");
    }
}

struct con_driver *con_driver_active(void) { return active; }

void con_driver_geometry(int *cols, int *rows) {
    if (cols) { *cols = con_cols; }
    if (rows) { *rows = con_rows; }
}

void con_driver_selftest(void) {
    if (!active) {
        serial_write_string("[con] selftest FAILED: no driver selected\n");
        return;
    }
    active->putc_attr('X', CON_WHITE);
    active->clear();
    serial_write_string("[con] selftest passed (");
    serial_write_string(active->name);
    serial_write_string(")\n");
}
