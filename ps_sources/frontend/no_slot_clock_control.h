#ifndef NO_SLOT_CLOCK_CONTROL_H
#define NO_SLOT_CLOCK_CONTROL_H

#include <stdint.h>

#include "../lib/rtc_pcf8563.h"

void no_slot_clock_control_init(void);
void no_slot_clock_control_set_enabled(uint8_t enable);
void no_slot_clock_control_publish_rtc(const rtc_pcf8563_time_t *time);
int no_slot_clock_control_poll_apple_write(XIicPs *i2c, rtc_pcf8563_time_t *rtc);

#endif /* NO_SLOT_CLOCK_CONTROL_H */
