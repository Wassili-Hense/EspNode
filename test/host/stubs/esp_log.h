#pragma once
#include <stdio.h>
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG } esp_log_level_t;
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do { (void)(tag); } while (0)
#define ESP_LOG_BUFFER_HEX_LEVEL(tag, buf, len, level) do { (void)(tag); (void)(buf); (void)(len); (void)(level); } while (0)
#define ESP_LOG_BUFFER_HEX(tag, buf, len) do { (void)(tag); (void)(buf); (void)(len); } while (0)
