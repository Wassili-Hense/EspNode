/*
 * PLC IO hardware layer on ESP-IDF, ESP32.
 */
#include <string.h>

#include "driver/dac_oneshot.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/uart_ll.h"
#include "soc/dac_channel.h"
#include "soc/soc_caps.h"

#include "plc_io_hw.h"

static const char *TAG = "plc_io";

#define ADC_DEFAULT_VREF    1100                    // mV, when eFuse has no calibration
#define PWM_MODE            LEDC_HIGH_SPEED_MODE    // ESP32: clock source per timer
#define PWM_APB_HZ          80000000
#define PWM_REF_TICK_HZ     1000000
#define PWM_APB_MIN_HZ      100                     // Lower frequencies from REF_TICK
#define CNT_HIGH_LIMIT      32767                   // 16-bit hardware counter, accumulated by the driver
#define CNT_LOW_LIMIT       (-32768)
#define CNT_FILTER_MAX_NS   12700                   // 1023 APB cycles
#define TWI_TIMEOUT_MS      200                     // Per transfer, below PLC_TWI_TIMEOUT_MS
#define TWI_TASK_STACK      3072
#define TWI_TASK_PRIO       5                       // As the node task
#define TWI_IDLE            (-2)                    // No transfer, or its result taken

// A TWI bus and the task running its transfers
typedef struct {
    TaskHandle_t                task;
    SemaphoreHandle_t           go;
    i2c_master_bus_handle_t     bus;
    i2c_master_dev_handle_t     dev;                // Address set before each transfer
    uint8_t                     addr;
    size_t                      tx_len;
    size_t                      rx_len;
    uint8_t                     tx[PLC_HW_TWI_SIZE];
    uint8_t                     rx[PLC_HW_TWI_SIZE];
    int                         result;             // PLC_HW_TWI_*, TWI_IDLE; shared with the task
} twi_t;

static struct {
    adc_oneshot_unit_handle_t   adc[2];             // ADC1, ADC2
    adc_cali_handle_t           cali[2][4];         // Per unit and attenuation
    dac_oneshot_handle_t        dac[2];
    uint8_t                     pwm_bits[PLC_HW_PWM_TIMERS];
    pcnt_unit_handle_t          cnt[PLC_HW_COUNTERS];
    pcnt_channel_handle_t       cnt_ch[PLC_HW_COUNTERS];
    twi_t                       twi[PLC_HW_TWI_BUSES];
} s;

static bool adc_channel(uint32_t pin, adc_unit_t *unit, adc_channel_t *ch)
{
    return (adc_oneshot_io_to_channel((int)pin, unit, ch) == ESP_OK) &&
           ((*unit == ADC_UNIT_1) || (*unit == ADC_UNIT_2));
}

static int dac_index(uint32_t pin)
{
    return (pin == DAC_CHAN0_GPIO_NUM) ? 0 : (pin == DAC_CHAN1_GPIO_NUM) ? 1 : -1;
}

uint32_t plc_hw_caps(uint32_t pin)
{
    if ((pin >= PLC_HW_PINS) || !GPIO_IS_VALID_GPIO((int)pin)) {
        return 0;
    }

    uint32_t caps = PLC_HW_CAP_VALID;
    if (GPIO_IS_VALID_OUTPUT_GPIO((int)pin)) {
        caps |= PLC_HW_CAP_OUTPUT;
    }
    adc_unit_t unit;
    adc_channel_t ch;
    if (adc_channel(pin, &unit, &ch)) {
        caps |= (unit == ADC_UNIT_1) ? PLC_HW_CAP_ADC : PLC_HW_CAP_ADC2;
    }
    if (dac_index(pin) >= 0) {
        caps |= PLC_HW_CAP_DAC;
    }
    return caps;
}

int64_t plc_hw_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// A released pin is a floating input: neither driven nor pulled
void plc_hw_release(uint32_t pin)
{
    gpio_reset_pin((gpio_num_t)pin);
    if (GPIO_IS_VALID_OUTPUT_GPIO((int)pin)) {
        gpio_pullup_dis((gpio_num_t)pin);   // gpio_reset_pin() enables it
    }
}

////////////////////////////////////////////////////////////////////////
// GPIO

bool plc_hw_gpio_input(uint32_t pin, int pull)
{
    const gpio_config_t cfg = {
        .pin_bit_mask   = 1ull << pin,
        .mode           = GPIO_MODE_INPUT,
        .pull_up_en     = (pull == 1) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en   = (pull == 2) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type      = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg) == ESP_OK;
}

bool plc_hw_gpio_output(uint32_t pin, bool open_drain, bool level)
{
    gpio_set_level((gpio_num_t)pin, level);     // Before the driver is on: no glitch
    const gpio_config_t cfg = {
        .pin_bit_mask   = 1ull << pin,
        .mode           = open_drain ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT,
        .pull_up_en     = GPIO_PULLUP_DISABLE,
        .pull_down_en   = GPIO_PULLDOWN_DISABLE,
        .intr_type      = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg) == ESP_OK;
}

bool plc_hw_gpio_get(uint32_t pin)
{
    return gpio_get_level((gpio_num_t)pin) != 0;
}

void plc_hw_gpio_set(uint32_t pin, bool level)
{
    gpio_set_level((gpio_num_t)pin, level);
}

////////////////////////////////////////////////////////////////////////
// ADC, DAC

bool plc_hw_adc_open(uint32_t pin, int atten)
{
    adc_unit_t unit;
    adc_channel_t ch;
    if (!adc_channel(pin, &unit, &ch) || (atten < 0) || (atten > 3)) {
        return false;
    }
    if (s.adc[unit] == NULL) {
        if ((s.adc[ADC_UNIT_1] == NULL) && (s.adc[ADC_UNIT_2] == NULL)) {
            // Without calibration in eFuse the reference may be 1000..1200 mV instead of 1100
            static const char *const source[] = { "eFuse Vref", "eFuse two point", "default Vref, no eFuse" };
            adc_cali_line_fitting_efuse_val_t val;
            if ((adc_cali_scheme_line_fitting_check_efuse(&val) == ESP_OK) && ((unsigned)val < 3)) {
                ESP_LOGI(TAG, "ADC calibration: %s", source[val]);
            }
        }
        const adc_oneshot_unit_init_cfg_t cfg = { .unit_id = unit };
        if (adc_oneshot_new_unit(&cfg, &s.adc[unit]) != ESP_OK) {
            return false;
        }
    }
    if (s.cali[unit][atten] == NULL) {
        const adc_cali_line_fitting_config_t cfg = {
            .unit_id        = unit,
            .atten          = (adc_atten_t)atten,
            .bitwidth       = ADC_BITWIDTH_DEFAULT,
            .default_vref   = ADC_DEFAULT_VREF,
        };
        if (adc_cali_create_scheme_line_fitting(&cfg, &s.cali[unit][atten]) != ESP_OK) {
            return false;
        }
    }
    const adc_oneshot_chan_cfg_t cfg = { .atten = (adc_atten_t)atten, .bitwidth = ADC_BITWIDTH_DEFAULT };
    return adc_oneshot_config_channel(s.adc[unit], ch, &cfg) == ESP_OK;
}

// ADC2 fails (ESP_ERR_TIMEOUT) while the WiFi driver holds it; plc_io does not allow it then
bool plc_hw_adc_read(uint32_t pin, int atten, int32_t *mv)
{
    adc_unit_t unit;
    adc_channel_t ch;
    int val = 0;
    if (!adc_channel(pin, &unit, &ch) ||
        (adc_oneshot_get_calibrated_result(s.adc[unit], s.cali[unit][atten], ch, &val) != ESP_OK)) {
        return false;
    }
    *mv = val;
    return true;
}

bool plc_hw_dac_open(uint32_t pin)
{
    int i = dac_index(pin);
    if (i < 0) {
        return false;
    }
    const dac_oneshot_config_t cfg = { .chan_id = (i == 0) ? DAC_CHAN_0 : DAC_CHAN_1 };
    if (dac_oneshot_new_channel(&cfg, &s.dac[i]) != ESP_OK) {
        return false;
    }
    dac_oneshot_output_voltage(s.dac[i], 0);
    return true;
}

void plc_hw_dac_write(uint32_t pin, uint8_t value)
{
    int i = dac_index(pin);
    if ((i >= 0) && (s.dac[i] != NULL)) {
        dac_oneshot_output_voltage(s.dac[i], value);
    }
}

void plc_hw_dac_close(uint32_t pin)
{
    int i = dac_index(pin);
    if ((i >= 0) && (s.dac[i] != NULL)) {
        dac_oneshot_del_channel(s.dac[i]);
        s.dac[i] = NULL;
    }
}

////////////////////////////////////////////////////////////////////////
// PWM

bool plc_hw_pwm_timer(int timer, uint32_t freq)
{
    bool slow = freq < PWM_APB_MIN_HZ;
    uint32_t bits = ledc_find_suitable_duty_resolution(slow ? PWM_REF_TICK_HZ : PWM_APB_HZ, freq);
    if (bits == 0) {
        return false;
    }
    if (bits > SOC_LEDC_TIMER_BIT_WIDTH) {
        bits = SOC_LEDC_TIMER_BIT_WIDTH;
    }

    const ledc_timer_config_t cfg = {
        .speed_mode         = PWM_MODE,
        .duty_resolution    = (ledc_timer_bit_t)bits,
        .timer_num          = (ledc_timer_t)timer,
        .freq_hz            = freq,
        .clk_cfg            = slow ? LEDC_USE_REF_TICK : LEDC_USE_APB_CLK,
    };
    if (ledc_timer_config(&cfg) != ESP_OK) {
        return false;
    }
    s.pwm_bits[timer] = (uint8_t)bits;
    return true;
}

bool plc_hw_pwm_open(int channel, int timer, uint32_t pin, bool invert)
{
    const ledc_channel_config_t cfg = {
        .gpio_num   = (int)pin,
        .speed_mode = PWM_MODE,
        .channel    = (ledc_channel_t)channel,
        .timer_sel  = (ledc_timer_t)timer,
        .duty       = 0,
        .hpoint     = 0,
        .flags.output_invert = invert,
    };
    return ledc_channel_config(&cfg) == ESP_OK;
}

void plc_hw_pwm_write(int channel, int timer, uint32_t duty)
{
    // 0..65535 to 0..2^bits, 2^bits is always on
    uint32_t val = (uint32_t)(((uint64_t)duty << s.pwm_bits[timer]) / 0xFFFF);
    ledc_set_duty(PWM_MODE, (ledc_channel_t)channel, val);
    ledc_update_duty(PWM_MODE, (ledc_channel_t)channel);
}

void plc_hw_pwm_close(int channel)
{
    ledc_stop(PWM_MODE, (ledc_channel_t)channel, 0);
    const ledc_channel_config_t cfg = {
        .speed_mode     = PWM_MODE,
        .channel        = (ledc_channel_t)channel,
        .deconfigure    = true,
    };
    ledc_channel_config(&cfg);
}

////////////////////////////////////////////////////////////////////////
// Pulse counter

bool plc_hw_cnt_open(int unit, uint32_t pin, int edges, uint32_t filter_ns)
{
    if (filter_ns > CNT_FILTER_MAX_NS) {
        return false;
    }

    const pcnt_unit_config_t ucfg = {
        .low_limit          = CNT_LOW_LIMIT,
        .high_limit         = CNT_HIGH_LIMIT,
        .flags.accum_count  = 1,
    };
    pcnt_unit_handle_t u = NULL;
    if (pcnt_new_unit(&ucfg, &u) != ESP_OK) {
        return false;
    }

    const pcnt_chan_config_t ccfg = { .edge_gpio_num = (int)pin, .level_gpio_num = -1 };
    const pcnt_glitch_filter_config_t fcfg = { .max_glitch_ns = filter_ns };
    pcnt_channel_handle_t ch = NULL;
    bool ok = (pcnt_new_channel(u, &ccfg, &ch) == ESP_OK) &&
              (pcnt_channel_set_edge_action(ch,
                   (edges != 1) ? PCNT_CHANNEL_EDGE_ACTION_INCREASE : PCNT_CHANNEL_EDGE_ACTION_HOLD,
                   (edges != 0) ? PCNT_CHANNEL_EDGE_ACTION_INCREASE : PCNT_CHANNEL_EDGE_ACTION_HOLD) == ESP_OK) &&
              ((filter_ns == 0) || (pcnt_unit_set_glitch_filter(u, &fcfg) == ESP_OK)) &&
              (pcnt_unit_add_watch_point(u, CNT_HIGH_LIMIT) == ESP_OK) &&     // Overflow accumulation
              (pcnt_unit_add_watch_point(u, CNT_LOW_LIMIT) == ESP_OK) &&
              (pcnt_unit_enable(u) == ESP_OK);
    if (!ok) {
        if (ch != NULL) {
            pcnt_del_channel(ch);
        }
        pcnt_del_unit(u);
        return false;
    }
    pcnt_unit_clear_count(u);
    pcnt_unit_start(u);

    if (GPIO_IS_VALID_OUTPUT_GPIO((int)pin)) {
        gpio_pullup_en((gpio_num_t)pin);    // Open collector and contact sensors
    }
    s.cnt[unit] = u;
    s.cnt_ch[unit] = ch;
    return true;
}

int32_t plc_hw_cnt_read(int unit)
{
    int val = 0;
    if (s.cnt[unit] != NULL) {
        pcnt_unit_get_count(s.cnt[unit], &val);
    }
    return val;
}

void plc_hw_cnt_close(int unit)
{
    if (s.cnt[unit] == NULL) {
        return;
    }
    pcnt_unit_stop(s.cnt[unit]);
    pcnt_unit_disable(s.cnt[unit]);
    pcnt_del_channel(s.cnt_ch[unit]);
    pcnt_del_unit(s.cnt[unit]);
    s.cnt[unit] = NULL;
    s.cnt_ch[unit] = NULL;
}

////////////////////////////////////////////////////////////////////////
// TWI: transfers run in a task per bus, so that the PLC cycle and MQTT-SN don't wait for them

static int twi_transfer(twi_t *t)
{
    esp_err_t err;
    if ((t->tx_len == 0) && (t->rx_len == 0)) {
        err = i2c_master_probe(t->bus, t->addr, TWI_TIMEOUT_MS);
    } else {
        err = i2c_master_device_change_address(t->dev, t->addr, TWI_TIMEOUT_MS);
        if (err != ESP_OK) {
            // Bus busy
        } else if (t->rx_len == 0) {
            err = i2c_master_transmit(t->dev, t->tx, t->tx_len, TWI_TIMEOUT_MS);
        } else if (t->tx_len == 0) {
            err = i2c_master_receive(t->dev, t->rx, t->rx_len, TWI_TIMEOUT_MS);
        } else {
            err = i2c_master_transmit_receive(t->dev, t->tx, t->tx_len, t->rx, t->rx_len, TWI_TIMEOUT_MS);
        }
    }

    switch (err) {
    case ESP_OK:
        return PLC_HW_TWI_OK;
    case ESP_ERR_NOT_FOUND:                 // Probe
    case ESP_ERR_INVALID_RESPONSE:          // Transfer
        return PLC_HW_TWI_NACK;
    case ESP_ERR_TIMEOUT:
        return PLC_HW_TWI_TIMEOUT;
    default:
        return PLC_HW_TWI_ERROR;
    }
}

static void twi_task(void *arg)
{
    twi_t *t = arg;
    for (;;) {
        xSemaphoreTake(t->go, portMAX_DELAY);
        int res = twi_transfer(t);
        __atomic_store_n(&t->result, res, __ATOMIC_RELEASE);     // rx before the result
    }
}

bool plc_hw_twi_open(int bus, uint32_t sda, uint32_t scl, uint32_t khz)
{
    twi_t *t = &s.twi[bus];
    if (t->task == NULL) {                  // Kept for later opens
        t->go = xSemaphoreCreateBinary();
        if ((t->go == NULL) ||
            (xTaskCreate(twi_task, "plc_twi", TWI_TASK_STACK, t, TWI_TASK_PRIO, &t->task) != pdPASS)) {
            return false;
        }
    }

    const i2c_master_bus_config_t bcfg = {
        .i2c_port           = bus,
        .sda_io_num         = (gpio_num_t)sda,
        .scl_io_num         = (gpio_num_t)scl,
        .clk_source         = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt  = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bcfg, &t->bus) != ESP_OK) {
        t->bus = NULL;
        return false;
    }

    const i2c_device_config_t dcfg = {
        .dev_addr_length    = I2C_ADDR_BIT_LEN_7,
        .device_address     = 0,
        .scl_speed_hz       = khz * 1000,
    };
    if (i2c_master_bus_add_device(t->bus, &dcfg, &t->dev) != ESP_OK) {
        i2c_del_master_bus(t->bus);
        t->bus = NULL;
        return false;
    }

    // A missing device is a status for the program, not a console error on every request
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    __atomic_store_n(&t->result, TWI_IDLE, __ATOMIC_RELAXED);
    return true;
}

bool plc_hw_twi_start(int bus, uint8_t addr, const uint8_t *tx, size_t tx_len, size_t rx_len)
{
    twi_t *t = &s.twi[bus];
    if ((t->bus == NULL) || (tx_len > PLC_HW_TWI_SIZE) || (rx_len > PLC_HW_TWI_SIZE) ||
        (__atomic_load_n(&t->result, __ATOMIC_ACQUIRE) == PLC_HW_TWI_BUSY)) {
        return false;
    }
    t->addr = addr;
    memcpy(t->tx, tx, tx_len);
    t->tx_len = tx_len;
    t->rx_len = rx_len;
    __atomic_store_n(&t->result, PLC_HW_TWI_BUSY, __ATOMIC_RELEASE);
    xSemaphoreGive(t->go);
    return true;
}

int plc_hw_twi_poll(int bus, uint8_t *rx)
{
    twi_t *t = &s.twi[bus];
    int res = __atomic_load_n(&t->result, __ATOMIC_ACQUIRE);
    if (res == PLC_HW_TWI_BUSY) {
        return res;
    }
    if (res == TWI_IDLE) {
        return PLC_HW_TWI_ERROR;            // Nothing was started
    }
    if (res == PLC_HW_TWI_OK) {
        memcpy(rx, t->rx, t->rx_len);
    }
    __atomic_store_n(&t->result, TWI_IDLE, __ATOMIC_RELAXED);
    return res;
}

void plc_hw_twi_close(int bus)
{
    twi_t *t = &s.twi[bus];
    if (t->bus == NULL) {
        return;
    }
    // The task uses the bus until the transfer ends, at most TWI_TIMEOUT_MS
    while (__atomic_load_n(&t->result, __ATOMIC_ACQUIRE) == PLC_HW_TWI_BUSY) {
        vTaskDelay(1);
    }
    i2c_master_bus_rm_device(t->dev);
    i2c_del_master_bus(t->bus);
    t->bus = NULL;
    t->dev = NULL;
}

////////////////////////////////////////////////////////////////////////
// UART: no TX ring buffer, bytes go straight to the hardware FIFO, so writes never block

bool plc_hw_uart_open(int port, uint32_t tx, uint32_t rx, int de, uint32_t baud, uint32_t format)
{
    static const uart_parity_t parity[] = { UART_PARITY_DISABLE, UART_PARITY_EVEN, UART_PARITY_ODD };
    if ((format & 3) > 2) {
        return false;
    }

    const uart_config_t cfg = {
        .baud_rate  = (int)baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = parity[format & 3],
        .stop_bits  = (format & 4) ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if ((uart_param_config((uart_port_t)port, &cfg) != ESP_OK) ||
        (uart_set_pin((uart_port_t)port, (int)tx, (int)rx, (de >= 0) ? de : UART_PIN_NO_CHANGE,
                      UART_PIN_NO_CHANGE) != ESP_OK) ||
        (uart_driver_install((uart_port_t)port, PLC_HW_UART_RX_SIZE, 0, 0, NULL, 0) != ESP_OK)) {
        return false;
    }
    // RS-485: the driver drives DE while the FIFO is sending
    if (uart_set_mode((uart_port_t)port, (de >= 0) ? UART_MODE_RS485_HALF_DUPLEX : UART_MODE_UART) != ESP_OK) {
        uart_driver_delete((uart_port_t)port);
        return false;
    }
    return true;
}

size_t plc_hw_uart_avail(int port)
{
    size_t len = 0;
    uart_get_buffered_data_len((uart_port_t)port, &len);
    return len;
}

int plc_hw_uart_get(int port)
{
    uint8_t byte;
    return (uart_read_bytes((uart_port_t)port, &byte, 1, 0) == 1) ? byte : -1;
}

bool plc_hw_uart_put(int port, uint8_t byte)
{
    if (uart_ll_get_txfifo_len(UART_LL_GET_HW(port)) == 0) {
        return false;                       // FIFO full, uart_write_bytes() would wait
    }
    return uart_write_bytes((uart_port_t)port, &byte, 1) == 1;
}

void plc_hw_uart_close(int port)
{
    if (uart_is_driver_installed((uart_port_t)port)) {
        uart_driver_delete((uart_port_t)port);
    }
}
