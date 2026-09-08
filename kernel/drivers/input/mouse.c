// kernel/drivers/input/mouse.c -- the PS/2 mouse packet decoder.
//
// State machine only. No port I/O and no event posting live here, which
// is what makes it testable the way keyboard_decode is: bytes in,
// packets out, no hardware and no device state. mouse_init and the IRQ
// handler are added in the next commit and sit on top of this.

#include "drivers/input/mouse.h"
#include "drivers/char/serial.h"
#include "drivers/input/input.h"
#include "arch/io.h"

static uint8_t pkt[4];
static int pkt_len = 3;      // 4 after a successful IntelliMouse knock
static int pkt_pos = 0;

void mouse_set_packet_size(int n) {
    pkt_len = (n == 4) ? 4 : 3;
    pkt_pos = 0;
}

int mouse_decode(uint8_t byte, struct mouse_packet *out) {
    // Bit 3 of the first byte is always 1 in a real PS/2 packet. If it
    // is not, the byte stream is out of sync -- drop the byte and stay
    // at position 0 rather than decode three bytes of garbage into a
    // cursor jump across the screen.
    if (pkt_pos == 0 && !(byte & 0x08)) { return 0; }

    pkt[pkt_pos++] = byte;
    if (pkt_pos < pkt_len) { return 0; }
    pkt_pos = 0;

    uint8_t flags = pkt[0];

    // Bits 6 and 7 are the X and Y overflow flags: the counters
    // saturated and the deltas mean nothing. Report the buttons and no
    // motion, which is a dropped movement -- far better than a jump.
    if (flags & 0xC0) {
        out->dx = out->dy = out->dwheel = 0;
    } else {
        int dx = pkt[1], dy = pkt[2];
        if (flags & 0x10) { dx |= ~0xFF; }   // sign lives in the flag byte
        if (flags & 0x20) { dy |= ~0xFF; }
        out->dx = (int16_t)dx;
        // PS/2 reports Y positive UPWARD; evdev's REL_Y is positive
        // downward. Linux negates here and so do we.
        out->dy = (int16_t)(-dy);

        out->dwheel = 0;
        if (pkt_len == 4) {
            // Z is a 4-bit signed field in the low nibble. Linux reports
            // REL_WHEEL as its negation, so a wire value of -1 is one
            // notch UP; matching that is the whole point of being
            // Linux-shaped here.
            int z = pkt[3] & 0x0F;
            if (z & 0x08) { z |= ~0x0F; }
            out->dwheel = (int16_t)(-z);
        }
    }
    out->buttons = flags & 0x07;
    return 1;
}

void mouse_decode_selftest(void) {
    struct mouse_packet p;

    mouse_set_packet_size(3);

    // +5 right, +5 up, no buttons. The first two bytes must not
    // complete a packet.
    if (mouse_decode(0x08, &p) || mouse_decode(0x05, &p)) {
        serial_write_string("[mouse] decode selftest FAILED: early complete\n");
        return;
    }
    if (!mouse_decode(0x05, &p) || p.dx != 5 || p.dy != -5 || p.buttons != 0) {
        serial_write_string("[mouse] decode selftest FAILED: simple move\n");
        return;
    }

    // Negative dx: X sign bit set in the flags, dx byte 0xFB.
    mouse_decode(0x18, &p); mouse_decode(0xFB, &p);
    if (!mouse_decode(0x00, &p) || p.dx != -5 || p.dy != 0) {
        serial_write_string("[mouse] decode selftest FAILED: negative dx\n");
        return;
    }

    // Left button held, no motion.
    mouse_decode(0x09, &p); mouse_decode(0x00, &p);
    if (!mouse_decode(0x00, &p) || p.buttons != 0x01 || p.dx != 0 || p.dy != 0) {
        serial_write_string("[mouse] decode selftest FAILED: left button\n");
        return;
    }

    // Overflow: buttons still reported, motion suppressed.
    mouse_decode(0x4A, &p); mouse_decode(0x7F, &p);
    if (!mouse_decode(0x7F, &p) || p.dx != 0 || p.dy != 0 || p.buttons != 0x02) {
        serial_write_string("[mouse] decode selftest FAILED: overflow\n");
        return;
    }

    // A first byte with bit 3 clear is not a valid header: drop it and
    // stay in sync, rather than start a packet on it.
    if (mouse_decode(0x00, &p)) {
        serial_write_string("[mouse] decode selftest FAILED: bad header accepted\n");
        return;
    }
    // The first two bytes of the recovered packet are incomplete (0);
    // only the third completes it.
    if (mouse_decode(0x08, &p) || mouse_decode(0x01, &p) ||
        !mouse_decode(0x00, &p) || p.dx != 1) {
        serial_write_string("[mouse] decode selftest FAILED: did not resync\n");
        return;
    }

    // 4-byte IntelliMouse. A wire Z of -1 (0xFF) is one notch UP, which
    // is REL_WHEEL +1 -- Linux's sign convention.
    mouse_set_packet_size(4);
    mouse_decode(0x08, &p); mouse_decode(0x00, &p); mouse_decode(0x00, &p);
    if (!mouse_decode(0xFF, &p) || p.dwheel != 1) {
        serial_write_string("[mouse] decode selftest FAILED: wheel up\n");
        return;
    }
    mouse_decode(0x08, &p); mouse_decode(0x00, &p); mouse_decode(0x00, &p);
    if (!mouse_decode(0x01, &p) || p.dwheel != -1) {
        serial_write_string("[mouse] decode selftest FAILED: wheel down\n");
        return;
    }
    mouse_set_packet_size(3);

    serial_write_string("[mouse] decode selftest passed\n");
}

// ---- the i8042 auxiliary port -----------------------------------------

#define PS2_DATA 0x60
#define PS2_CMD  0x64
#define PS2_STAT 0x64

// Bounded spins: a machine with no aux port must not wedge the boot.
static int ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STAT) & 0x02)) { return 1; }
    }
    return 0;
}

static int ps2_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STAT) & 0x01) { return 1; }
    }
    return 0;
}

// A command for the MOUSE has to be introduced with 0xD4; without the
// prefix the controller hands the byte to the keyboard instead.
static int aux_cmd(uint8_t cmd, uint8_t *ack) {
    if (!ps2_wait_write()) { return 0; }
    outb(PS2_CMD, 0xD4);
    if (!ps2_wait_write()) { return 0; }
    outb(PS2_DATA, cmd);
    if (!ps2_wait_read()) { return 0; }
    uint8_t r = inb(PS2_DATA);
    if (ack) { *ack = r; }
    return r == 0xFA;                    // 0xFA is ACK
}

int mouse_init(void) {
    input_dev_init(&input_mouse, "NeoOS PS/2 mouse");
    input_mouse.ev_bits  = (1u << EV_SYN) | (1u << EV_KEY) | (1u << EV_REL);
    input_mouse.rel_bits = (1u << REL_X) | (1u << REL_Y) | (1u << REL_WHEEL);

    if (!ps2_wait_write()) { return 0; }
    outb(PS2_CMD, 0xA8);                 // enable the aux port

    // Turn the aux interrupt (IRQ12) on in the controller config byte,
    // and clear the aux clock-disable bit while we are here.
    if (!ps2_wait_write()) { return 0; }
    outb(PS2_CMD, 0x20);
    if (!ps2_wait_read()) { return 0; }
    uint8_t cfg = inb(PS2_DATA);
    cfg |= 0x02;
    cfg &= (uint8_t)~0x20;
    if (!ps2_wait_write()) { return 0; }
    outb(PS2_CMD, 0x60);
    if (!ps2_wait_write()) { return 0; }
    outb(PS2_DATA, cfg);

    if (!aux_cmd(0xF6, 0)) { return 0; }  // set defaults

    // The IntelliMouse knock: sample rates 200, 100, 80 in that order
    // make a wheel mouse start answering 0xF2 with id 3 and switch to
    // 4-byte packets. A plain mouse keeps answering 0.
    uint8_t id = 0;
    if (aux_cmd(0xF3, 0)) { aux_cmd(200, 0); }
    if (aux_cmd(0xF3, 0)) { aux_cmd(100, 0); }
    if (aux_cmd(0xF3, 0)) { aux_cmd(80, 0); }
    if (aux_cmd(0xF2, 0) && ps2_wait_read()) { id = inb(PS2_DATA); }
    mouse_set_packet_size(id == 3 ? 4 : 3);

    if (!aux_cmd(0xF4, 0)) { return 0; }  // enable reporting

    serial_write_string("[mouse] ps/2 aux port ready, packet=");
    serial_write_hex64(id == 3 ? 4 : 3);
    serial_write_string("\n");
    return 1;
}

// Turn a decoded packet into its evdev event group and post it. Only
// axes that MOVED and buttons that CHANGED are reported, which is what
// Linux does and what keeps a resting mouse silent.
void mouse_post_packet(const struct mouse_packet *p) {
    static uint8_t prev_buttons;
    struct input_event ev[8];
    int n = 0;

    if (p->dx)     { ev[n].type = EV_REL; ev[n].code = REL_X;     ev[n].value = p->dx;     n++; }
    if (p->dy)     { ev[n].type = EV_REL; ev[n].code = REL_Y;     ev[n].value = p->dy;     n++; }
    if (p->dwheel) { ev[n].type = EV_REL; ev[n].code = REL_WHEEL; ev[n].value = p->dwheel; n++; }

    static const uint16_t btn[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    for (int i = 0; i < 3; i++) {
        uint8_t was = prev_buttons & (uint8_t)(1u << i);
        uint8_t now = p->buttons   & (uint8_t)(1u << i);
        if (was != now) {
            ev[n].type = EV_KEY; ev[n].code = btn[i]; ev[n].value = now ? 1 : 0; n++;
        }
    }
    prev_buttons = p->buttons;

    if (n == 0) { return; }              // nothing changed; stay silent
    ev[n].type = EV_SYN; ev[n].code = SYN_REPORT; ev[n].value = 0; n++;
    input_post(&input_mouse, ev, n);
}

void mouse_handler(void) {
    struct mouse_packet p;
    if (mouse_decode(inb(PS2_DATA), &p)) { mouse_post_packet(&p); }
}
