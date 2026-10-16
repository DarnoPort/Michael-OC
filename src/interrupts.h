#ifndef MICHAEL_OC_INTERRUPTS_H
#define MICHAEL_OC_INTERRUPTS_H

void interrupts_init(void);

/* Called only by the assembly exception entry stubs. Never returns. */
void interrupt_exception_handler(unsigned int vector, unsigned int error_code)
    __attribute__((noreturn));

#endif
