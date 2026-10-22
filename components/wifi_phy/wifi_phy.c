/*
 * MQTT-SN transport over UDP on WiFi.
 */
#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "mqttsn_defs.h"
#include "wifi_phy.h"

static const char *TAG = "wifi_phy";

#define RX_BUF_SIZE         128         // Longer datagrams are not MQTT-SN of X13, dropped
#define RX_TASK_STACK       3072
#define RX_TASK_PRIO        10
#define RECONNECT_MIN_MS    1000
#define RECONNECT_MAX_MS    60000

static struct {
    int                 sock;
    uint16_t            gw_port;
    wifi_phy_rx_cb_t    rx_cb;
    wifi_phy_ip_cb_t    ip_cb;
    void               *arg;
    esp_timer_handle_t  reconnect;
    uint32_t            delay_ms;       // Pause before the next connection attempt
    bool                has_ip;
    uint8_t             ip[4];          // Own address while has_ip, own broadcasts are dropped
} s;

static bool phy_send(void *ctx, const mqttsn_addr_t *dst, const uint8_t *msg, size_t len)
{
    (void)ctx;
    if (dst->len != 4) {
        return false;
    }
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(s.gw_port) };
    memcpy(&to.sin_addr.s_addr, dst->b, 4);
    return sendto(s.sock, msg, len, 0, (struct sockaddr *)&to, sizeof(to)) == (int)len;
}

static const mqttsn_transport_t s_transport = {
    .ctx        = NULL,
    .broadcast  = { .len = 4, .b = { 255, 255, 255, 255 } },
    .undef      = { .len = 4, .b = { 0, 0, 0, 0 } },
    .send       = phy_send,
    .set_addr   = NULL,                 // The address is the IP of the station
};

static void rx_task(void *arg)
{
    uint8_t buf[RX_BUF_SIZE];

    (void)arg;
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s.sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            ESP_LOGE(TAG, "recvfrom: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        mqttsn_addr_t src = { .len = 4 };
        memcpy(src.b, &from.sin_addr.s_addr, 4);
        if ((n < 2) || (n > MQTTSN_LINK_MAX_LEN) || (s.has_ip && (memcmp(src.b, s.ip, 4) == 0))) {
            continue;
        }
        s.rx_cb(&src, buf, (size_t)n, s.arg);
    }
}

static void reconnect(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if ((base == WIFI_EVENT) && (id == WIFI_EVENT_STA_START)) {
        esp_wifi_connect();
    } else if ((base == WIFI_EVENT) && (id == WIFI_EVENT_STA_DISCONNECTED)) {
        const wifi_event_sta_disconnected_t *e = data;
        ESP_LOGW(TAG, "disconnected (reason %d), next attempt in %" PRIu32 " ms", e->reason, s.delay_ms);
        if (s.has_ip) {
            s.has_ip = false;
            s.ip_cb(NULL, s.arg);
        }
        esp_timer_start_once(s.reconnect, (uint64_t)s.delay_ms * 1000);
        s.delay_ms = (s.delay_ms * 2 < RECONNECT_MAX_MS) ? s.delay_ms * 2 : RECONNECT_MAX_MS;
    } else if ((base == IP_EVENT) && (id == IP_EVENT_STA_GOT_IP)) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "IP " IPSTR, IP2STR(&e->ip_info.ip));
        memcpy(s.ip, &e->ip_info.ip.addr, 4);
        s.has_ip = true;
        s.delay_ms = RECONNECT_MIN_MS;
        s.ip_cb(s.ip, s.arg);
    } else if ((base == IP_EVENT) && (id == IP_EVENT_STA_LOST_IP)) {
        ESP_LOGW(TAG, "IP lost");
        if (s.has_ip) {
            s.has_ip = false;
            s.ip_cb(NULL, s.arg);
        }
    }
}

static void set_ipv4(esp_ip4_addr_t *dst, const uint8_t *b)
{
    memcpy(&dst->addr, b, 4);
}

esp_err_t wifi_phy_init(const wifi_phy_config_t *cfg, wifi_phy_rx_cb_t rx_cb, wifi_phy_ip_cb_t ip_cb, void *arg)
{
    ESP_RETURN_ON_FALSE((rx_cb != NULL) && (ip_cb != NULL), ESP_ERR_INVALID_ARG, TAG, "callbacks are required");

    s.gw_port = cfg->gw_port;
    s.rx_cb = rx_cb;
    s.ip_cb = ip_cb;
    s.arg = arg;
    s.delay_ms = RECONNECT_MIN_MS;

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    esp_err_t err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE((err == ESP_OK) || (err == ESP_ERR_INVALID_STATE), err, TAG, "event loop");
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(netif != NULL, ESP_FAIL, TAG, "netif");
    if (cfg->static_ip) {
        esp_netif_ip_info_t info;
        set_ipv4(&info.ip, cfg->ip);
        set_ipv4(&info.netmask, cfg->mask);
        set_ipv4(&info.gw, cfg->gateway);
        ESP_RETURN_ON_ERROR(esp_netif_dhcpc_stop(netif), TAG, "dhcpc_stop");
        ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(netif, &info), TAG, "set_ip_info");
    }

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");    // No copy of the password in NVS
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL),
                        TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL),
                        TAG, "ip events");

    wifi_config_t wcfg = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,            // Strongest of several access points
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };
    memcpy(wcfg.sta.ssid, cfg->ssid, strnlen(cfg->ssid, sizeof(wcfg.sta.ssid)));
    memcpy(wcfg.sta.password, cfg->password, strnlen(cfg->password, sizeof(wcfg.sta.password)));
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wcfg), TAG, "set_config");

    const esp_timer_create_args_t timer = { .callback = reconnect, .name = "wifi_reconnect" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer, &s.reconnect), TAG, "timer");

    s.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_RETURN_ON_FALSE(s.sock >= 0, ESP_FAIL, TAG, "socket: errno %d", errno);
    int on = 1;
    setsockopt(s.sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));     // SEARCHGW
    const struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(WIFI_PHY_PORT),
                                       .sin_addr.s_addr = htonl(INADDR_ANY) };
    ESP_RETURN_ON_FALSE(bind(s.sock, (const struct sockaddr *)&local, sizeof(local)) == 0, ESP_FAIL, TAG,
                        "bind: errno %d", errno);
    BaseType_t ok = xTaskCreate(rx_task, "udp_rx", RX_TASK_STACK, NULL, RX_TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "rx task");

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
    // No power save: a node answers the gateway at once, not at the next beacon
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set_ps");

    ESP_LOGI(TAG, "SSID '%s', %s, gateway port %u", cfg->ssid, cfg->static_ip ? "static IP" : "DHCP",
             (unsigned)cfg->gw_port);
    return ESP_OK;
}

const mqttsn_transport_t *wifi_phy_transport(void)
{
    return &s_transport;
}
