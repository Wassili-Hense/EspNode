/*
 * Wall clock record of the X13 RTC object (OD_ID_RTC), as the STM32 RTC registers:
 * seconds, minutes, hours, day, weekday << 5 | month, year - 2000; BCD.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define OD_CLOCK_BCD_SIZE   6

void od_clock_get_bcd(uint8_t *bcd);

// false - not a valid date and time, the clock is not changed
bool od_clock_set_bcd(const uint8_t *bcd);
