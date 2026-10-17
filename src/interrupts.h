#ifndef MICHAEL_OC_INTERRUPTS_H
#define MICHAEL_OC_INTERRUPTS_H

#include "multiboot.h"

void interrupts_init(void);
void interrupts_enable(void);

/* Non-blocking read from the IRQ1 producer / kernel-loop consumer queue. */
int keyboard_try_get_scancode(u8 *scancode);

typedef struct {
    int dx;
    int dy;
    u8 buttons;
} mouse_event_t;

/* Read decoded three-byte PS/2 mouse packets queued by IRQ12. */
int mouse_try_get_event(mouse_event_t *event);
int mouse_is_available(void);

/* PIT channel 0 runs at approximately 100 Hz when initialized successfully. */
u32 timer_ticks(void);

/* Called by assembly exception/IRQ entry stubs. */
void interrupt_exception_handler(u32 vector, u32 error_code)
    __attribute__((noreturn));
void interrupt_irq_handler(u32 irq_number);

#endif
