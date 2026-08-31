#include "dev/pic.h"
#include "arch/io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

void pic_disable(void) {
    outb(PIC1_CMD, 0x11); // begin init sequence, cascade mode
    outb(PIC2_CMD, 0x11);
    outb(PIC1_DATA, 0x20); // remap IRQ0-7 to vectors 0x20-0x27 (defensive: off the exception range)
    outb(PIC2_DATA, 0x28); // remap IRQ8-15 to vectors 0x28-0x2F
    outb(PIC1_DATA, 0x04); // PIC1 has a slave on IRQ2
    outb(PIC2_DATA, 0x02); // PIC2's cascade identity
    outb(PIC1_DATA, 0x01); // 8086 mode
    outb(PIC2_DATA, 0x01);
    outb(PIC1_DATA, 0xFF); // mask every line
    outb(PIC2_DATA, 0xFF);
}
