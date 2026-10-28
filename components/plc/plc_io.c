/*
 * PLC IO logic: pin modes, resources, program restarts. Hardware access goes through
 * plc_io_hw.h only, so this file runs in host tests as is.
 */
#include <stdbool.h>
#include <string.h>

#include "plc_io.h"
#include "plc_io_hw.h"

#define TWI_KHZ_DEFAULT     100
#define TWI_KHZ_MAX         1000
#define UART_BAUD_DEFAULT   9600
#define UART_BAUD_MAX       5000000
#define PWM_FREQ_DEFAULT    1000
#define DEBOUNCE_MAX_MS     60000
#define FREE                (-1)

typedef enum {
    GRP_NONE,
    GRP_IN,
    GRP_OUT,
    GRP_ADC,
    GRP_DAC,
    GRP_PWM,
    GRP_CNT,
    GRP_TWI,
    GRP_UART,
} group_t;

// TWI transaction failed: closed by the TwiStatus that reports it
#define TWI_FAILED          (PLC_TWI_WD | PLC_TWI_SLANACK | PLC_TWI_ERROR)

typedef struct {
    uint8_t     mode;           // PLC_PIN_*, PLC_PIN_OFF - free
    bool        stale;          // Configured by the previous run, not configured again yet
    bool        raw;            // Input: last level read
    int8_t      res;            // PWM channel or counter unit
    int32_t     option;
    uint32_t    value;          // Input: debounced level; output, DAC, PWM: last written
    int32_t     offset;         // Counter: value = hardware count + offset
    int64_t     changed_ms;     // Input: when the level last changed
} pin_t;

typedef struct {
    bool        open;
    bool        active;         // Transaction open, X13: pTWI != NULL
    bool        want_start;     // Bytes to write complete, the hardware is still busy
    uint8_t     addr;           // Control word / status fields
    uint8_t     access;
    uint8_t     write;
    uint8_t     read;
    uint8_t     pnt;            // Bytes put by the program / taken by it
    int64_t     start_ms;
    uint8_t     data[PLC_HW_TWI_SIZE];      // To write, then read
} twi_t;

static struct {
    uint64_t    reserved_pins;
    uint8_t     reserved_uarts;
    bool        adc2;
    pin_t       pin[PLC_HW_PINS];
    struct {
        int8_t  pin;
        int8_t  timer;
    } pwm[PLC_HW_PWM_CHANNELS];
    struct {
        uint32_t freq;
        uint8_t  users;
    } timer[PLC_HW_PWM_TIMERS];
    int8_t      cnt[PLC_HW_COUNTERS];   // Owner pin
    twi_t       twi[PLC_HW_TWI_BUSES];
    struct {
        bool    open;
        size_t  seen;           // Bytes in the receive buffer at the previous UartBytesToRead
    } uart[PLC_HW_UART_PORTS];
} s;

////////////////////////////////////////////////////////////////////////
// Modes

static group_t group_of(uint32_t mode)
{
    switch (mode) {
    case PLC_PIN_IN:
    case PLC_PIN_IN_PU:
    case PLC_PIN_IN_PD:
    case PLC_PIN_IN_INV_PU:
        return GRP_IN;
    case PLC_PIN_OUT:
    case PLC_PIN_OUT_INV:
    case PLC_PIN_OUT_OD:
    case PLC_PIN_OUT_OD_INV:
        return GRP_OUT;
    case PLC_PIN_ANALOG:
        return GRP_ADC;
    case PLC_PIN_DAC:
        return GRP_DAC;
    case PLC_PIN_PWM:
    case PLC_PIN_PWM_INV:
        return GRP_PWM;
    case PLC_PIN_CNT_RISE:
    case PLC_PIN_CNT_FALL:
    case PLC_PIN_CNT_BOTH:
        return GRP_CNT;
    case PLC_PIN_TWI0_SDA:
    case PLC_PIN_TWI0_SCL:
    case PLC_PIN_TWI1_SDA:
    case PLC_PIN_TWI1_SCL:
        return GRP_TWI;
    case PLC_PIN_UART1_TX:
    case PLC_PIN_UART1_RX:
    case PLC_PIN_UART1_DE:
    case PLC_PIN_UART2_TX:
    case PLC_PIN_UART2_RX:
    case PLC_PIN_UART2_DE:
        return GRP_UART;
    default:
        return GRP_NONE;
    }
}

static bool inverted(uint32_t mode)
{
    return (mode == PLC_PIN_IN_INV_PU) || (mode == PLC_PIN_OUT_INV) || (mode == PLC_PIN_OUT_OD_INV) ||
           (mode == PLC_PIN_PWM_INV);
}

// TWI: bus 0/1; UART: port 1/2
static int twi_bus(uint32_t mode)     { return (int)(mode - PLC_PIN_TWI0_SDA) / 2; }
static int uart_port(uint32_t mode)   { return 1 + (int)(mode - PLC_PIN_UART1_TX) / 3; }

static uint32_t twi_mode(int bus, bool scl)
{
    return PLC_PIN_TWI0_SDA + (uint32_t)bus * 2 + (scl ? 1 : 0);
}

static uint32_t uart_mode(int port, int role)       // role: 0 TX, 1 RX, 2 DE
{
    return PLC_PIN_UART1_TX + (uint32_t)(port - 1) * 3 + (uint32_t)role;
}

static int find_pin(uint32_t mode)
{
    for (int i = 0; i < PLC_HW_PINS; i++) {
        if (s.pin[i].mode == mode) {
            return i;
        }
    }
    return FREE;
}

////////////////////////////////////////////////////////////////////////
// Buses: opened when all their pins are configured

// false - the hardware refused the pins or the settings
static bool twi_try_open(int bus)
{
    int sda = find_pin(twi_mode(bus, false));
    int scl = find_pin(twi_mode(bus, true));
    if (s.twi[bus].open || (sda == FREE) || (scl == FREE)) {
        return true;
    }
    int32_t khz = s.pin[scl].option ? s.pin[scl].option : TWI_KHZ_DEFAULT;
    memset(&s.twi[bus], 0, sizeof(s.twi[bus]));
    s.twi[bus].open = plc_hw_twi_open(bus, (uint32_t)sda, (uint32_t)scl, (uint32_t)khz);
    return s.twi[bus].open;
}

static void twi_close(int bus)
{
    if (s.twi[bus].open) {
        plc_hw_twi_close(bus);
        memset(&s.twi[bus], 0, sizeof(s.twi[bus]));
    }
}

// (Re)open with the current pins, also when DE was added or removed; false - the hardware refused
static bool uart_reopen(int port)
{
    if (s.uart[port].open) {
        plc_hw_uart_close(port);
        s.uart[port].open = false;
    }
    s.uart[port].seen = 0;

    int tx = find_pin(uart_mode(port, 0));
    int rx = find_pin(uart_mode(port, 1));
    int de = find_pin(uart_mode(port, 2));
    if ((tx == FREE) || (rx == FREE)) {
        return true;
    }
    int32_t baud = s.pin[tx].option ? s.pin[tx].option : UART_BAUD_DEFAULT;
    s.uart[port].open = plc_hw_uart_open(port, (uint32_t)tx, (uint32_t)rx, de, (uint32_t)baud,
                                         (uint32_t)s.pin[rx].option);
    return s.uart[port].open;
}

////////////////////////////////////////////////////////////////////////
// Pin setup and release

static void release_pin(uint32_t pin)
{
    pin_t *p = &s.pin[pin];
    uint32_t mode = p->mode;

    p->mode = PLC_PIN_OFF;          // Before reopening a bus without this pin

    switch (group_of(mode)) {
    case GRP_DAC:
        plc_hw_dac_close(pin);
        break;
    case GRP_PWM:
        plc_hw_pwm_close(p->res);
        s.timer[s.pwm[p->res].timer].users--;
        s.pwm[p->res].pin = FREE;
        s.pwm[p->res].timer = FREE;
        break;
    case GRP_CNT:
        plc_hw_cnt_close(p->res);
        s.cnt[p->res] = FREE;
        break;
    case GRP_TWI:
        twi_close(twi_bus(mode));
        break;
    case GRP_UART:
        (void)uart_reopen(uart_port(mode)); // Closes; reopens if only DE was removed
        break;
    default:
        break;
    }

    plc_hw_release(pin);
    memset(p, 0, sizeof(*p));
}

// Program restart: the pins of the previous run keep their resources until the end of the init
// cycle. Release them earlier when the new run needs the resources.
static bool release_stale(group_t grp)
{
    bool any = false;
    for (uint32_t i = 0; i < PLC_HW_PINS; i++) {
        if (s.pin[i].stale && (group_of(s.pin[i].mode) == grp)) {
            release_pin(i);
            any = true;
        }
    }
    return any;
}

// Timer running at freq or a free one, and a free channel
static bool find_pwm(uint32_t freq, int *timer, int *ch)
{
    *timer = FREE;
    for (int i = 0; i < PLC_HW_PWM_TIMERS; i++) {
        if ((s.timer[i].users > 0) && (s.timer[i].freq == freq)) {
            *timer = i;
            break;
        }
        if ((*timer == FREE) && (s.timer[i].users == 0)) {
            *timer = i;
        }
    }
    *ch = FREE;
    for (int i = 0; (i < PLC_HW_PWM_CHANNELS) && (*ch == FREE); i++) {
        if (s.pwm[i].pin == FREE) {
            *ch = i;
        }
    }
    return (*timer != FREE) && (*ch != FREE);
}

static int find_cnt(void)
{
    for (int i = 0; i < PLC_HW_COUNTERS; i++) {
        if (s.cnt[i] == FREE) {
            return i;
        }
    }
    return FREE;
}

static plc_status_t setup_pwm(uint32_t pin, uint32_t mode, int32_t option)
{
    if (option < 0) {
        return PLC_ERR_PARM;
    }
    uint32_t freq = option ? (uint32_t)option : PWM_FREQ_DEFAULT;

    int timer, ch;
    if (!find_pwm(freq, &timer, &ch) && (!release_stale(GRP_PWM) || !find_pwm(freq, &timer, &ch))) {
        return PLC_ERR_RSC_BUSY;
    }

    if ((s.timer[timer].users == 0) && !plc_hw_pwm_timer(timer, freq)) {
        return PLC_ERR_PARM;                // Frequency not possible
    }
    if (!plc_hw_pwm_open(ch, timer, pin, inverted(mode))) {
        return PLC_ERR_PARM;
    }

    s.timer[timer].freq = freq;
    s.timer[timer].users++;
    s.pwm[ch].pin = (int8_t)pin;
    s.pwm[ch].timer = (int8_t)timer;
    s.pin[pin].res = (int8_t)ch;
    return PLC_RUN;
}

static plc_status_t setup_cnt(uint32_t pin, uint32_t mode, int32_t option)
{
    if (option < 0) {
        return PLC_ERR_PARM;
    }
    int unit = find_cnt();
    if ((unit == FREE) && release_stale(GRP_CNT)) {
        unit = find_cnt();
    }
    if (unit == FREE) {
        return PLC_ERR_RSC_BUSY;
    }
    if (!plc_hw_cnt_open(unit, pin, (int)(mode - PLC_PIN_CNT_RISE), (uint32_t)option)) {
        return PLC_ERR_PARM;
    }
    s.cnt[unit] = (int8_t)pin;
    s.pin[pin].res = (int8_t)unit;
    return PLC_RUN;
}

// A bus pin role (e.g. TWI0 SCL) belongs to one pin at a time
static plc_status_t claim_role(uint32_t pin, uint32_t mode)
{
    int other = find_pin(mode);
    if ((other == FREE) || ((uint32_t)other == pin)) {
        return PLC_RUN;
    }
    if (!s.pin[other].stale) {
        return PLC_ERR_RSC_BUSY;
    }
    release_pin((uint32_t)other);
    return PLC_RUN;
}

static plc_status_t setup(uint32_t pin, uint32_t mode, int32_t option)
{
    uint32_t caps = plc_hw_caps(pin);
    pin_t *p = &s.pin[pin];

    switch (group_of(mode)) {
    case GRP_IN: {
        int pull = (mode == PLC_PIN_IN) ? 0 : (mode == PLC_PIN_IN_PD) ? 2 : 1;
        if (((pull != 0) && !(caps & PLC_HW_CAP_OUTPUT)) || (option < 0) || (option > DEBOUNCE_MAX_MS) ||
            !plc_hw_gpio_input(pin, pull)) {
            return PLC_ERR_PARM;
        }
        p->raw = plc_hw_gpio_get(pin) ^ inverted(mode);
        p->value = p->raw;
        p->changed_ms = plc_hw_now_ms();
        return PLC_RUN;
    }

    case GRP_OUT: {
        bool od = (mode == PLC_PIN_OUT_OD) || (mode == PLC_PIN_OUT_OD_INV);
        if (!(caps & PLC_HW_CAP_OUTPUT) || !plc_hw_gpio_output(pin, od, inverted(mode))) {
            return PLC_ERR_PARM;            // Starts logically off
        }
        p->value = 0;
        return PLC_RUN;
    }

    case GRP_ADC: {
        bool adc = (caps & PLC_HW_CAP_ADC) || (s.adc2 && (caps & PLC_HW_CAP_ADC2));
        if (!adc || (option < 0) || (option > 3) || !plc_hw_adc_open(pin, (int)option)) {
            return PLC_ERR_PARM;
        }
        return PLC_RUN;
    }

    case GRP_DAC:
        if (!(caps & PLC_HW_CAP_DAC) || !plc_hw_dac_open(pin)) {
            return PLC_ERR_PARM;
        }
        p->value = 0;
        return PLC_RUN;

    case GRP_PWM:
        if (!(caps & PLC_HW_CAP_OUTPUT)) {
            return PLC_ERR_PARM;
        }
        p->value = 0;
        return setup_pwm(pin, mode, option);

    case GRP_CNT:
        p->offset = 0;
        return setup_cnt(pin, mode, option);

    case GRP_TWI:
        if (!(caps & PLC_HW_CAP_OUTPUT) || (option < 0) || (option > TWI_KHZ_MAX)) {
            return PLC_ERR_PARM;
        }
        return claim_role(pin, mode);

    case GRP_UART: {
        int port = uart_port(mode);
        int role = (int)(mode - PLC_PIN_UART1_TX) % 3;
        if (s.reserved_uarts & (1u << port)) {
            return PLC_ERR_RSC_BUSY;        // Used by the MQTT-SN link
        }
        if (((role != 1) && !(caps & PLC_HW_CAP_OUTPUT)) || (option < 0) ||
            ((role == 0) && (option > UART_BAUD_MAX)) ||
            ((role == 1) && ((option > 7) || ((option & 3) == 3)))) {
            return PLC_ERR_PARM;
        }
        return claim_role(pin, mode);
    }

    default:
        return PLC_ERR_PARM;
    }
}

////////////////////////////////////////////////////////////////////////
// API

void plc_io_init(const plc_io_config_t *cfg)
{
    memset(&s, 0, sizeof(s));
    s.reserved_pins = cfg->reserved_pins;
    s.reserved_uarts = cfg->reserved_uarts;
    s.adc2 = cfg->adc2;
    for (int i = 0; i < PLC_HW_PWM_CHANNELS; i++) {
        s.pwm[i].pin = FREE;
        s.pwm[i].timer = FREE;
    }
    for (int i = 0; i < PLC_HW_COUNTERS; i++) {
        s.cnt[i] = FREE;
    }
}

void plc_io_restart_begin(void)
{
    for (int i = 0; i < PLC_HW_PINS; i++) {
        if (s.pin[i].mode != PLC_PIN_OFF) {
            s.pin[i].stale = true;
        }
    }
    for (int i = 0; i < PLC_HW_TWI_BUSES; i++) {
        s.twi[i].active = false;            // Transaction of the stopped program
        s.twi[i].want_start = false;
    }
}

void plc_io_restart_end(void)
{
    for (uint32_t i = 0; i < PLC_HW_PINS; i++) {
        if ((s.pin[i].mode != PLC_PIN_OFF) && s.pin[i].stale) {
            release_pin(i);
        }
    }
}

plc_status_t plc_io_config(uint32_t pin, uint32_t mode, int32_t option)
{
    if ((pin >= PLC_HW_PINS) || !(plc_hw_caps(pin) & PLC_HW_CAP_VALID) ||
        (s.reserved_pins & (1ull << pin)) || (mode >= PLC_PIN_MODES)) {
        return PLC_ERR_PARM;
    }

    pin_t *p = &s.pin[pin];
    if (mode == PLC_PIN_OFF) {
        if (p->mode != PLC_PIN_OFF) {
            release_pin(pin);
        }
        return PLC_RUN;
    }

    if (p->mode != PLC_PIN_OFF) {
        if ((p->mode == mode) && (p->option == option)) {
            p->stale = false;               // Same setup: keep running untouched
            return PLC_RUN;
        }
        if (!p->stale) {
            return PLC_ERR_RSC_BUSY;        // Used by another function of this program
        }
        release_pin(pin);
    }

    plc_status_t stat = setup(pin, mode, option);
    if (stat != PLC_RUN) {
        plc_hw_release(pin);
        memset(p, 0, sizeof(*p));
        return stat;
    }

    p->mode = (uint8_t)mode;
    p->option = option;
    p->stale = false;

    bool bus_ok = true;
    if (group_of(mode) == GRP_TWI) {
        bus_ok = twi_try_open(twi_bus(mode));
    } else if (group_of(mode) == GRP_UART) {
        bus_ok = uart_reopen(uart_port(mode));
    }
    if (!bus_ok) {
        release_pin(pin);
        return PLC_ERR_PARM;
    }
    return PLC_RUN;
}

plc_status_t plc_io_in(uint32_t pin, uint32_t *value)
{
    if (pin >= PLC_HW_PINS) {
        return PLC_ERR_PARM;
    }
    pin_t *p = &s.pin[pin];

    switch (group_of(p->mode)) {
    case GRP_IN: {
        bool raw = plc_hw_gpio_get(pin) ^ inverted(p->mode);
        int64_t now = plc_hw_now_ms();
        if (raw != p->raw) {
            p->raw = raw;
            p->changed_ms = now;
        }
        if ((p->value != raw) && (now - p->changed_ms >= p->option)) {
            p->value = raw;
        }
        *value = p->value;
        return PLC_RUN;
    }
    case GRP_OUT:
    case GRP_DAC:
    case GRP_PWM:
        *value = p->value;
        return PLC_RUN;
    case GRP_ADC: {
        int32_t mv = 0;
        if (!plc_hw_adc_read(pin, (int)p->option, &mv)) {
            return PLC_ERR_RSC_BUSY;
        }
        *value = (uint32_t)mv;
        return PLC_RUN;
    }
    case GRP_CNT:
        *value = (uint32_t)(plc_hw_cnt_read(p->res) + p->offset);
        return PLC_RUN;
    default:
        return PLC_ERR_PARM;                // Not configured or a bus pin
    }
}

plc_status_t plc_io_out(uint32_t pin, uint32_t value)
{
    if (pin >= PLC_HW_PINS) {
        return PLC_ERR_PARM;
    }
    pin_t *p = &s.pin[pin];

    switch (group_of(p->mode)) {
    case GRP_OUT:
        value = (value != 0);
        if (value != p->value) {
            p->value = value;
            plc_hw_gpio_set(pin, (value != 0) ^ inverted(p->mode));
        }
        return PLC_RUN;
    case GRP_DAC:
        if (value > 255) {
            value = 255;
        }
        if (value != p->value) {
            p->value = value;
            plc_hw_dac_write(pin, (uint8_t)value);
        }
        return PLC_RUN;
    case GRP_PWM:
        if (value > 0xFFFF) {
            value = 0xFFFF;
        }
        if (value != p->value) {
            p->value = value;
            plc_hw_pwm_write(p->res, s.pwm[p->res].timer, value);
        }
        return PLC_RUN;
    case GRP_CNT:
        p->offset = (int32_t)value - plc_hw_cnt_read(p->res);
        return PLC_RUN;
    default:
        return PLC_ERR_PARM;
    }
}

////////////////////////////////////////////////////////////////////////
// TWI, as X13 exttwi.c

static bool twi_ready(uint32_t bus)
{
    return (bus < PLC_HW_TWI_BUSES) && s.twi[bus].open;
}

static void twi_try_start(int bus)
{
    if (s.twi[bus].want_start &&
        plc_hw_twi_start(bus, s.twi[bus].addr, s.twi[bus].data, s.twi[bus].write, s.twi[bus].read)) {
        s.twi[bus].want_start = false;
        s.twi[bus].access |= PLC_TWI_BUSY;
    }
}

// Move an open transaction on: start the transfer when the hardware is free, take its result,
// watch the time
static void twi_update(int bus)
{
    twi_t *t = &s.twi[bus];
    if (!t->active || (t->access & (PLC_TWI_RDY | TWI_FAILED))) {
        return;
    }

    twi_try_start(bus);
    if (t->access & PLC_TWI_BUSY) {
        switch (plc_hw_twi_poll(bus, t->data)) {
        case PLC_HW_TWI_BUSY:
            break;
        case PLC_HW_TWI_OK:
            t->access = PLC_TWI_RDY;        // X13: access flags cleared
            t->pnt = 0;
            return;
        case PLC_HW_TWI_NACK:
            t->access = (uint8_t)((t->access & ~PLC_TWI_BUSY) | PLC_TWI_SLANACK);
            t->read = 0;
            return;
        case PLC_HW_TWI_TIMEOUT:
            t->access = (uint8_t)((t->access & ~PLC_TWI_BUSY) | PLC_TWI_WD);
            return;
        default:
            t->access = (uint8_t)((t->access & ~PLC_TWI_BUSY) | PLC_TWI_ERROR);
            return;
        }
    }

    // Bytes to write never came, or the hardware is stuck. A transfer still running ends by its own
    // timeout; a new one starts after it.
    if (plc_hw_now_ms() - t->start_ms > PLC_TWI_TIMEOUT_MS) {
        t->access |= PLC_TWI_WD;
    }
}

plc_status_t plc_io_twi_control(uint32_t bus, uint32_t ctrl)
{
    if (!twi_ready(bus)) {
        return PLC_ERR_PARM;
    }
    twi_t *t = &s.twi[bus];
    if (t->active) {
        return PLC_RUN;                     // X13: ignored while a transaction is open
    }

    uint32_t addr = ctrl & 0xFF;
    uint32_t write = (ctrl >> 16) & 0xFF;
    uint32_t read = ctrl >> 24;
    if ((addr > 0x7F) || (write > PLC_HW_TWI_SIZE) || (read > PLC_HW_TWI_SIZE)) {
        return PLC_ERR_PARM;
    }

    t->active = true;
    t->addr = (uint8_t)addr;
    t->write = (uint8_t)write;
    t->read = (uint8_t)read;
    t->access = (uint8_t)((write ? PLC_TWI_WRITE : 0) | (read ? PLC_TWI_READ : 0));
    t->pnt = 0;
    t->start_ms = plc_hw_now_ms();
    t->want_start = (write == 0);           // Otherwise the last TwiPutByte starts it
    twi_try_start((int)bus);
    return PLC_RUN;
}

plc_status_t plc_io_twi_status(uint32_t bus, uint32_t *status)
{
    if (!twi_ready(bus)) {
        return PLC_ERR_PARM;
    }
    twi_t *t = &s.twi[bus];
    if (!t->active) {
        *status = 0;                        // Bus free
        return PLC_RUN;
    }

    twi_update((int)bus);
    *status = t->addr | ((uint32_t)t->access << 8) | ((uint32_t)t->write << 16) | ((uint32_t)t->read << 24);
    if (t->access & TWI_FAILED) {
        t->active = false;
    } else if (t->access & PLC_TWI_RDY) {
        t->pnt = 0;
        if (t->read == 0) {
            t->active = false;
        }
    }
    return PLC_RUN;
}

plc_status_t plc_io_twi_put(uint32_t bus, uint32_t byte)
{
    if (!twi_ready(bus)) {
        return PLC_ERR_PARM;
    }
    twi_t *t = &s.twi[bus];
    // X13: ignored without a transaction waiting for bytes
    if (!t->active || t->want_start || (t->access & (PLC_TWI_BUSY | PLC_TWI_RDY | TWI_FAILED)) ||
        (t->pnt >= t->write)) {
        return PLC_RUN;
    }
    t->data[t->pnt++] = (uint8_t)byte;
    if (t->pnt == t->write) {
        t->want_start = true;
        twi_try_start((int)bus);
    }
    return PLC_RUN;
}

plc_status_t plc_io_twi_get(uint32_t bus, uint32_t *byte)
{
    if (!twi_ready(bus)) {
        return PLC_ERR_PARM;
    }
    twi_t *t = &s.twi[bus];
    twi_update((int)bus);
    *byte = 0;                              // X13: 0 when there is nothing to read
    if (t->active && (t->access & PLC_TWI_RDY)) {
        if (t->pnt < t->read) {
            *byte = t->data[t->pnt++];
        }
        if (t->pnt == t->read) {
            t->active = false;
        }
    }
    return PLC_RUN;
}

////////////////////////////////////////////////////////////////////////
// UART, as X13 extser.c

static bool uart_ready(uint32_t port)
{
    return (port >= 1) && (port < PLC_HW_UART_PORTS) && s.uart[port].open;
}

plc_status_t plc_io_uart_avail(uint32_t port, uint32_t *count)
{
    if (!uart_ready(port)) {
        return PLC_ERR_PARM;
    }
    // A frame is complete when nothing new arrived since the previous call
    size_t n = plc_hw_uart_avail((int)port);
    bool quiet = (n == s.uart[port].seen);
    s.uart[port].seen = n;
    *count = (quiet || (n >= PLC_HW_UART_RX_SIZE / 2)) ? (uint32_t)n : 0;
    return PLC_RUN;
}

plc_status_t plc_io_uart_get(uint32_t port, uint32_t *value)
{
    if (!uart_ready(port)) {
        return PLC_ERR_PARM;
    }
    int byte = plc_hw_uart_get((int)port);
    if ((byte >= 0) && (s.uart[port].seen > 0)) {
        s.uart[port].seen--;                // Taken by the program, not a new arrival
    }
    *value = (uint32_t)byte;
    return PLC_RUN;
}

plc_status_t plc_io_uart_put(uint32_t port, uint32_t byte, uint32_t *ok)
{
    if (!uart_ready(port)) {
        return PLC_ERR_PARM;
    }
    *ok = plc_hw_uart_put((int)port, (uint8_t)byte) ? 1 : 0;
    return PLC_RUN;
}
