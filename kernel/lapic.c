#include "lapic.h"
#include "mm/paging.h"

#define LAPIC_REG_ID         0x020
#define LAPIC_REG_EOI        0x0B0
#define LAPIC_REG_SVR        0x0F0
#define LAPIC_REG_LVT_TIMER  0x320
#define LAPIC_REG_TIMER_INIT 0x380
#define LAPIC_REG_TIMER_CUR  0x390
#define LAPIC_REG_TIMER_DIV  0x3E0

#define LAPIC_REG_ICR_LOW    0x300
#define LAPIC_REG_ICR_HIGH   0x310

#define ICR_DELIVERY_INIT    (5u << 8)
#define ICR_DELIVERY_STARTUP (6u << 8)
#define ICR_DELIVERY_NMI     (4u << 8)
#define ICR_LEVEL_ASSERT     (1u << 14)
#define ICR_TRIGGER_LEVEL    (1u << 15)
#define ICR_DELIVERY_PENDING (1u << 12)

#define LVT_MASKED         (1u << 16)
#define LVT_TIMER_PERIODIC (1u << 17)

static volatile uint32_t *lapic_base;

static uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t value) {
    lapic_base[reg / 4] = value;
}

void lapic_init(uint32_t address) {
    lapic_base = (volatile uint32_t *)phys_to_virt(address);
    lapic_write(LAPIC_REG_SVR, lapic_read(LAPIC_REG_SVR) | 0x100 | 0xFF); // software-enable, spurious vector 0xFF
}

static void lapic_wait_idle(void) {
    while (lapic_read(LAPIC_REG_ICR_LOW) & ICR_DELIVERY_PENDING) {
        __asm__ volatile ("pause");
    }
}

// ICR_HIGH selects the target and ICR_LOW fires the IPI, so ICR_HIGH
// must ALWAYS be written first -- writing LOW is the trigger.
static void lapic_send_icr(uint32_t lapic_id, uint32_t low) {
    lapic_write(LAPIC_REG_ICR_HIGH, lapic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, low);
    lapic_wait_idle();
}

void lapic_send_init(uint32_t lapic_id) {
    lapic_send_icr(lapic_id, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT | ICR_TRIGGER_LEVEL);
    lapic_send_icr(lapic_id, ICR_DELIVERY_INIT | ICR_TRIGGER_LEVEL); // deassert
}

// The SIPI vector IS the physical page number of the trampoline.
void lapic_send_sipi(uint32_t lapic_id, uint8_t vector) {
    lapic_send_icr(lapic_id, ICR_DELIVERY_STARTUP | ICR_LEVEL_ASSERT | vector);
}

void lapic_send_ipi(uint32_t lapic_id, uint8_t vector) {
    lapic_send_icr(lapic_id, ICR_LEVEL_ASSERT | vector);
}

// NMI, not a maskable vector: the point is to reach a CPU spinning with
// interrupts disabled, which a normal IPI cannot do.
void lapic_send_nmi(uint32_t lapic_id) {
    lapic_send_icr(lapic_id, ICR_DELIVERY_NMI | ICR_LEVEL_ASSERT);
}

// Software-enables the calling CPU's own LAPIC. The MMIO base is
// per-CPU aliased by hardware at the same address, so an AP needs only
// the SVR write -- lapic_base is already set by the BSP.
void lapic_init_this_cpu(void) {
    lapic_write(LAPIC_REG_SVR, lapic_read(LAPIC_REG_SVR) | 0x100 | 0xFF);
}

void lapic_send_eoi(void) {
    lapic_write(LAPIC_REG_EOI, 0);
}

uint32_t lapic_get_id(void) {
    return lapic_read(LAPIC_REG_ID) >> 24;
}

void lapic_timer_start_oneshot_max(void) {
    lapic_write(LAPIC_REG_TIMER_DIV, 0x3); // divide by 16
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFF);
}

uint32_t lapic_timer_stop_and_read(void) {
    uint32_t current = lapic_read(LAPIC_REG_TIMER_CUR);
    lapic_write(LAPIC_REG_TIMER_INIT, 0);
    return current;
}

void lapic_timer_start_periodic(uint32_t initial_count, uint8_t vector) {
    lapic_write(LAPIC_REG_TIMER_DIV, 0x3); // divide by 16
    lapic_write(LAPIC_REG_LVT_TIMER, (uint32_t)vector | LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_REG_TIMER_INIT, initial_count);
}
