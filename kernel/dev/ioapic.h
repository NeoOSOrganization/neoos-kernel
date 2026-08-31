#ifndef NEOOS_IOAPIC_H
#define NEOOS_IOAPIC_H

#include <stdint.h>

void ioapic_init(uint32_t address);
void ioapic_set_redirection(uint8_t pin, uint8_t vector, uint8_t polarity, uint8_t trigger, uint8_t dest_apic_id);

#endif
