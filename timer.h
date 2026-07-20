#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void timer_install(void);
void timer_wait(uint32_t ticks);
int timer_sleep(uint32_t ticks);
uint32_t timer_get_ticks(void);
uint32_t timer_get_sleeping_count(void);

#endif
