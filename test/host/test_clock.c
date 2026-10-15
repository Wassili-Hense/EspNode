// Host tests of the wall clock: calendar, running time, BCD record of the X13 RTC object
#include <stdio.h>
#include <string.h>

#include "objdict.h"
#include "od_clock.h"

#define S       1000000LL       // Microseconds

static int failed, passed;
static int64_t s_us;            // esp_timer_get_time()

int64_t esp_timer_get_time(void) { return s_us; }

#define CHECK(cond) do { \
    if (cond) { passed++; } else { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static bool now_is(int year, int month, int day, int wday, int hour, int min, int sec)
{
    od_clock_t c;
    od_clock_now(&c);
    bool ok = (c.year == year) && (c.month == month) && (c.day == day) && (c.wday == wday) &&
              (c.hour == hour) && (c.min == min) && (c.sec == sec);
    if (!ok) {
        printf("  clock: %04u-%02u-%02u wday %u %02u:%02u:%02u\n", c.year, c.month, c.day, c.wday,
               c.hour, c.min, c.sec);
    }
    return ok;
}

// As Enviriot sends it: sec, min, hour, day, weekday << 5 | month, year % 100, BCD
static bool set(uint8_t sec, uint8_t min, uint8_t hour, uint8_t day, uint8_t month, uint8_t year)
{
    const uint8_t bcd[OD_CLOCK_BCD_SIZE] = { sec, min, hour, day, (uint8_t)((1 << 5) | month), year };
    return od_clock_set_bcd(bcd);
}

int main(void)
{
    uint8_t bcd[OD_CLOCK_BCD_SIZE];

    // Until the gateway sets it: 1970-01-01 00:00:00 at boot, a Thursday
    s_us = 0;
    CHECK(now_is(1970, 1, 1, 4, 0, 0, 0));
    s_us = (86400 + 3661) * S;
    CHECK(now_is(1970, 1, 2, 5, 1, 1, 1));
    od_clock_get_bcd(bcd);
    CHECK(memcmp(bcd, (const uint8_t[]){ 0x01, 0x01, 0x01, 0x02, (5 << 5) | 0x01, 0x70 }, 6) == 0);

    // Set: the weekday is computed, not taken from the record
    CHECK(set(0x56, 0x34, 0x12, 0x15, 0x10, 0x26));
    CHECK(now_is(2026, 10, 15, 4, 12, 34, 56));
    od_clock_get_bcd(bcd);
    CHECK(memcmp(bcd, (const uint8_t[]){ 0x56, 0x34, 0x12, 0x15, (4 << 5) | 0x10, 0x26 }, 6) == 0);

    // Runs with the time since boot, the second starts when the record arrived
    s_us += 3 * S + S / 2;
    CHECK(now_is(2026, 10, 15, 4, 12, 34, 59));
    s_us += S / 2;
    CHECK(now_is(2026, 10, 15, 4, 12, 35, 0));

    // Day, month, year and century change
    CHECK(set(0x59, 0x59, 0x23, 0x31, 0x12, 0x99));
    s_us += S;
    CHECK(now_is(2100, 1, 1, 5, 0, 0, 0));
    od_clock_get_bcd(bcd);
    CHECK(bcd[5] == 0x00);
    CHECK(set(0x59, 0x59, 0x23, 0x29, 0x02, 0x00));     // 2000 is a leap year
    s_us += S;
    CHECK(now_is(2000, 3, 1, 3, 0, 0, 0));

    // Leap day, PM flag of the 12-hour format
    CHECK(set(0x00, 0x00, 0x40 | 0x01, 0x29, 0x02, 0x24));
    CHECK(now_is(2024, 2, 29, 4, 13, 0, 0));

    // Invalid records leave the clock as it is
    CHECK(!set(0x00, 0x00, 0x00, 0x29, 0x02, 0x23));    // 2023-02-29
    CHECK(!set(0x00, 0x00, 0x00, 0x31, 0x04, 0x26));    // April 31
    CHECK(!set(0x00, 0x00, 0x24, 0x01, 0x01, 0x26));    // Hour 24
    CHECK(!set(0x00, 0x60, 0x00, 0x01, 0x01, 0x26));    // Minute 60
    CHECK(!set(0x60, 0x00, 0x00, 0x01, 0x01, 0x26));    // Second 60
    CHECK(!set(0x00, 0x00, 0x00, 0x01, 0x13, 0x26));    // Month 13
    CHECK(!set(0x00, 0x00, 0x00, 0x01, 0x00, 0x26));    // Month 0
    CHECK(!set(0x00, 0x00, 0x00, 0x00, 0x01, 0x26));    // Day 0
    CHECK(!set(0x0A, 0x00, 0x00, 0x01, 0x01, 0x26));    // Not BCD
    CHECK(!set(0x00, 0x00, 0x00, 0x01, 0x01, 0xA0));
    CHECK(now_is(2024, 2, 29, 4, 13, 0, 0));

    printf("%d passed, %d failed\n", passed, failed);
    printf(failed ? "FAILED\n" : "ALL PASSED\n");
    return failed ? 1 : 0;
}
