// Host node for the end-to-end tests: real mqttsn/objdict/serial_frame/plc code, simulated time,
// in-memory NVS and program storage, fake IO hardware.
// Line protocol on stdin: "rx <hex>", "tick <n>", "status", "reboot", "packtest", "hw" (IO calls
// since the last "hw"), "level <pin>", "set <pin> <0|1>", "quit".
// Output: "tx <hex frame>" lines, other results, then "ok".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "fake_hw.h"
#include "mqttsn_client.h"
#include "nvs.h"
#include "objdict.h"
#include "od_pack.h"
#include "plc.h"
#include "serial_frame.h"

// ---------------- fake NVS
typedef struct { char key[16]; uint8_t data[512]; size_t len; int used; } kv_t;
static kv_t kv[16];

const char *esp_err_to_name(esp_err_t e) { static char b[16]; snprintf(b, sizeof b, "0x%x", e); return b; }
static kv_t *kv_find(const char *key) { for (int i = 0; i < 16; i++) if (kv[i].used && !strcmp(kv[i].key, key)) return &kv[i]; return NULL; }
static esp_err_t kv_set(const char *key, const void *v, size_t len)
{
    kv_t *e = kv_find(key);
    for (int i = 0; !e && i < 16; i++) if (!kv[i].used) e = &kv[i];
    if (!e || len > sizeof(e->data)) return ESP_FAIL;
    snprintf(e->key, sizeof e->key, "%s", key); memcpy(e->data, v, len); e->len = len; e->used = 1;
    return ESP_OK;
}
static esp_err_t kv_get(const char *key, void *v, size_t len)
{
    kv_t *e = kv_find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (e->len != len) return ESP_FAIL;
    memcpy(v, e->data, len); return ESP_OK;
}
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; (void)m; *h = 1; return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v) { (void)h; return kv_get(k, v, 1); }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *v) { (void)h; return kv_get(k, v, 2); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len)
{
    (void)h; kv_t *e = kv_find(k);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (e->len > *len) { *len = e->len; return ESP_FAIL; }
    memcpy(v, e->data, e->len); *len = e->len; return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { (void)h; return kv_set(k, &v, 1); }
esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v) { (void)h; return kv_set(k, &v, 2); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len) { (void)h; return kv_set(k, v, len); }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) { (void)h; kv_t *e = kv_find(k); if (!e) return ESP_ERR_NVS_NOT_FOUND; e->used = 0; return ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t h) { (void)h; memset(kv, 0, sizeof kv); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

// ---------------- platform
static int64_t sim_ms;
static uint32_t rng = 12345;

static int64_t now_ms(void) { return sim_ms; }
static uint16_t rand16(void) { rng = rng * 1103515245u + 12345u; return (uint16_t)(rng >> 8); }
int64_t esp_timer_get_time(void) { return sim_ms * 1000; }
uint32_t esp_random(void) { rng = rng * 1103515245u + 12345u; return rng; }

static bool phy_send(void *ctx, const mqttsn_addr_t *dst, const uint8_t *msg, size_t len)
{
    (void)ctx; (void)dst;
    uint8_t frame[SERIAL_FRAME_ENCODED_MAX(MQTTSN_MAX_MSG_LEN)];
    size_t n = serial_frame_encode(msg, len, frame, sizeof frame);
    printf("tx ");
    for (size_t i = 0; i < n; i++) printf("%02x", frame[i]);
    printf("\n");
    return n > 0;
}

static const mqttsn_transport_t transport = {
    .broadcast = { .len = 1, .b = { 0x00 } },
    .undef = { .len = 1, .b = { 0xFF } },
    .send = phy_send,
    .set_addr = NULL,
};

static void start(void)
{
    fake_reset();
    fake_now_ms = sim_ms;
    const plc_config_t plc_cfg = {
        .reserved_pins = 0xFC0ull | (1ull << 1) | (1ull << 3), .reserved_uarts = 1u << 1,
    };
    if (plc_init(&plc_cfg) != ESP_OK) { fprintf(stderr, "plc_init failed\n"); exit(1); }
    static const od_provider_t *providers[1];
    providers[0] = plc_provider();
    const od_config_t od_cfg = { .device_type = "E1S10.400", .providers = providers, .n_providers = 1 };
    if (od_init(&od_cfg) != ESP_OK) { fprintf(stderr, "od_init failed\n"); exit(1); }
    const mqttsn_client_config_t cfg = {
        .transport = &transport,
        .cb = { od_on_register, od_on_regack, od_on_publish, od_get_client_id, od_load_addrs, od_store_node_addr },
        .now_ms = now_ms, .rand16 = rand16,
    };
    mqttsn_client_init(&cfg);
}

// ---------------- od_pack checks
static int check(const char *name, uint8_t width, const uint8_t *in, uint8_t in_len, const uint8_t *exp, uint8_t exp_len, int pack)
{
    uint8_t buf[16] = {0};
    memcpy(buf, in, in_len);
    uint8_t n = pack ? od_pack(width, buf, in_len) : od_unpack(width, buf, in_len);
    if (n != exp_len || memcmp(buf, exp, n)) {
        printf("packtest FAIL %s: len %u\n", name, n);
        return 1;
    }
    return 0;
}

static int packtest(void)
{
    int f = 0;
    // pack
    f += check("i16 -1",     0x82, (uint8_t[]){0xFF, 0xFF}, 2, (uint8_t[]){0xFF}, 1, 1);
    f += check("i16 128",    0x82, (uint8_t[]){0x80, 0x00}, 2, (uint8_t[]){0x80, 0x00}, 2, 1);
    f += check("i16 -129",   0x82, (uint8_t[]){0x7F, 0xFF}, 2, (uint8_t[]){0x7F, 0xFF}, 2, 1);
    f += check("i16 5",      0x82, (uint8_t[]){0x05, 0x00}, 2, (uint8_t[]){0x05}, 1, 1);
    f += check("u16 255",    0x02, (uint8_t[]){0xFF, 0x00}, 2, (uint8_t[]){0xFF, 0x00}, 2, 1);
    f += check("u16 0xFFFF", 0x02, (uint8_t[]){0xFF, 0xFF}, 2, (uint8_t[]){0xFF, 0xFF, 0x00}, 3, 1);
    f += check("u8 0x80",    0x01, (uint8_t[]){0x80}, 1, (uint8_t[]){0x80, 0x00}, 2, 1);
    f += check("u8 0",       0x01, (uint8_t[]){0x00}, 1, (uint8_t[]){0x00}, 1, 1);
    f += check("i32 0x12345678", 0x84, (uint8_t[]){0x78, 0x56, 0x34, 0x12}, 4, (uint8_t[]){0x78, 0x56, 0x34, 0x12}, 4, 1);
    f += check("raw",        0x00, (uint8_t[]){1, 2, 3}, 3, (uint8_t[]){1, 2, 3}, 3, 1);
    // unpack
    f += check("un i16 -1",  0x82, (uint8_t[]){0xFF}, 1, (uint8_t[]){0xFF, 0xFF}, 2, 0);
    f += check("un u16 1",   0x02, (uint8_t[]){0x01}, 1, (uint8_t[]){0x01, 0x00}, 2, 0);
    f += check("un u16 ffff",0x02, (uint8_t[]){0xFF, 0xFF, 0x00}, 3, (uint8_t[]){0xFF, 0xFF}, 2, 0);
    f += check("un u8 empty",0x01, (uint8_t[]){0}, 0, (uint8_t[]){0x00}, 1, 0);
    f += check("un i64 -2",  0x88, (uint8_t[]){0xFE}, 1, (uint8_t[]){0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 8, 0);
    if (!f) printf("packtest ok\n");
    return f;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    start();

    serial_frame_decoder_t dec;
    serial_frame_decoder_reset(&dec);
    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "rx ", 3)) {
            for (const char *p = line + 3; p[0] && p[1]; p += 2) {
                unsigned b; sscanf(p, "%2x", &b);
                size_t len = serial_frame_decode(&dec, (uint8_t)b);
                if (len) {
                    mqttsn_client_handle_rx(NULL, dec.buf, len);
                }
            }
        } else if (!strncmp(line, "tick ", 5)) {
            int n = atoi(line + 5);
            for (int i = 0; i < n; i++) { sim_ms += 10; fake_now_ms = sim_ms; od_poll(); mqttsn_client_tick(); }
        } else if (!strcmp(line, "status")) {
            printf("status %s\n", mqttsn_client_status_str(mqttsn_client_status()));
        } else if (!strcmp(line, "reboot")) {
            start();
        } else if (!strcmp(line, "packtest")) {
            packtest();
        } else if (!strcmp(line, "hw")) {
            for (char *p = fake_calls; *p; ) {
                char *e = strchr(p, '\n');
                printf("hw %.*s\n", (int)(e - p), p);
                p = e + 1;
            }
            fake_clear();
        } else if (!strncmp(line, "level ", 6)) {
            printf("level %d\n", fake_level[atoi(line + 6) % PLC_HW_PINS]);
        } else if (!strncmp(line, "set ", 4)) {
            int pin = 0, level = 0;
            sscanf(line + 4, "%d %d", &pin, &level);
            fake_level[pin % PLC_HW_PINS] = level != 0;
        } else if (!strcmp(line, "quit")) {
            break;
        }
        printf("ok\n");
    }
    return 0;
}
