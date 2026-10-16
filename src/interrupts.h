#ifndef MICHAEL_OC_INTERRUPTS_H
#define MICHAEL_OC_INTERRUPTS_H

#include "multiboot.h"

void interrupts_init(void);
int interrupts_enable_keyboard_irq(void);
int interrupts_read_keyboard_scancode(u8 *scancode);

#endif
