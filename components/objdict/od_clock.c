/*
 * Wall clock: local time written by the gateway, counted on top of the time since boot.
 *
 * The system time of ESP-IDF is left alone: the gateway sends local time, not UTC, and the system
 * time must stay UTC for WiFi and TLS.
 */
#include "esp_timer.h"

#include "objdict.h"
#include "od_clock.h"

#define US_PER_S        1000000LL
#define S_PER_DAY       86400

static int64_t s_offset_us;     // Wall clock since 1970-01-01 00:00:00 minus time since boot

// Days since 1970-01-01, proleptic Gregorian calendar (H. Hinnant, days_from_civil)
static int64_t days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    int era = ((y >= 0) ? y : (y - 399)) / 400;
    int yoe = y - era * 400;                                        // [0, 399]
    int doy = (153 * ((m > 2) ? (m - 3) : (m + 9)) + 2) / 5 + d - 1; // [0, 365]
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                // [0, 146096]
    return (int64_t)era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, od_clock_t *c)
{
    z += 719468;
    int64_t era = ((z >= 0) ? z : (z - 146096)) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    c->day = (uint8_t)(doy - (153 * mp + 2) / 5 + 1);
    c->month = (uint8_t)((mp < 10) ? (mp + 3) : (mp - 9));
    c->year = (uint16_t)(yoe + era * 400 + (c->month <= 2));
}

void od_clock_now(od_clock_t *now)
{
    int64_t t = (esp_timer_get_time() + s_offset_us) / US_PER_S;
    int64_t days = t / S_PER_DAY;
    int sec = (int)(t % S_PER_DAY);

    civil_from_days(days, now);
    now->wday = (uint8_t)((days + 3) % 7 + 1);     // 1970-01-01 was a Thursday
    now->hour = (uint8_t)(sec / 3600);
    now->min = (uint8_t)(sec / 60 % 60);
    now->sec = (uint8_t)(sec % 60);
}

static uint8_t to_bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

// -1 - not BCD
static int from_bcd(uint8_t b)
{
    return (((b >> 4) > 9) || ((b & 0x0F) > 9)) ? -1 : (b >> 4) * 10 + (b & 0x0F);
}

void od_clock_get_bcd(uint8_t *bcd)
{
    od_clock_t c;
    od_clock_now(&c);
    bcd[0] = to_bcd(c.sec);
    bcd[1] = to_bcd(c.min);
    bcd[2] = to_bcd(c.hour);
    bcd[3] = to_bcd(c.day);
    bcd[4] = (uint8_t)((c.wday << 5) | to_bcd(c.month));
    bcd[5] = to_bcd(c.year % 100);
}

bool od_clock_set_bcd(const uint8_t *bcd)
{
    int sec = from_bcd(bcd[0] & 0x7F);
    int min = from_bcd(bcd[1] & 0x7F);
    int hour = from_bcd(bcd[2] & 0x3F);
    int day = from_bcd(bcd[3] & 0x3F);
    int month = from_bcd(bcd[4] & 0x1F);                // Weekday is computed, not taken
    int year = from_bcd(bcd[5]);
    if ((sec < 0) || (min < 0) || (hour < 0) || (day < 0) || (month < 0) || (year < 0)) {
        return false;
    }
    if (bcd[2] & 0x40) {
        hour += 12;                                     // PM, 12-hour format of the STM32 RTC
    }
    year += 2000;
    if ((sec > 59) || (min > 59) || (hour > 23) || (month < 1) || (month > 12) || (day < 1)) {
        return false;
    }

    int64_t days = days_from_civil(year, month, day);
    od_clock_t check;
    civil_from_days(days, &check);
    if (check.day != day) {
        return false;                                   // E.g. February 30
    }

    int64_t t = days * S_PER_DAY + hour * 3600 + min * 60 + sec;
    s_offset_us = t * US_PER_S - esp_timer_get_time();
    return true;
}
