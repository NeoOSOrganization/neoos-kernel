#ifndef NEOOS_LAPIC_H
#define NEOOS_LAPIC_H

#include <stdint.h>

void lapic_init(uint32_t address);
void lapic_init_this_cpu(void);
void lapic_send_init(uint32_t lapic_id);
void lapic_send_sipi(uint32_t lapic_id, uint8_t vector);
void lapic_send_ipi(uint32_t lapic_id, uint8_t vector);
void lapic_send_nmi(uint32_t lapic_id);
void lapic_send_eoi(void);
uint32_t lapic_get_id(void);
void lapic_timer_start_oneshot_max(void);
uint32_t lapic_timer_stop_and_read(void);
void lapic_timer_start_periodic(uint32_t initial_count, uint8_t vector);

#endif
