#ifndef MICHAEL_OC_INTERRUPTS_H
#define MICHAEL_OC_INTERRUPTS_H

#include "multiboot.h"

void interrupts_init(void);
int interrupts_enable_timer_keyboard_irqs(void);
u32 interrupts_timer_ticks(void);
int interrupts_read_keyboard_scancode(u8 *scancode);

#endif
