// kernel/drivers/input/mouse.c -- the PS/2 mouse packet decoder.
//
// State machine only. No port I/O and no event posting live here, which
// is what makes it testable the way keyboard_decode is: bytes in,
// packets out, no hardware and no device state. mouse_init and the IRQ
// handler are added in the next commit and sit on top of this.

#include "drivers/input/mouse.h"
#include "drivers/char/serial.h"

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
