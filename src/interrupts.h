#ifndef MICHAEL_OC_INTERRUPTS_H
#define MICHAEL_OC_INTERRUPTS_H

/* Installs protected-mode exception gates 0..31. IRQs remain disabled. */
void interrupts_init(void);

#endif
