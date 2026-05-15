/**
 * @file debug.h
 * @brief Conditional debug logging macros
 *
 * Controlled by CONFIG_LINKEY_DEBUG_LOGS Kconfig option.
 * When disabled, macros compile to no-ops with zero overhead.
 */

#ifndef DEBUG_H
#define DEBUG_H

#ifdef CONFIG_LINKEY_DEBUG_LOGS
    #include "esp_log.h"
    #define DEBUG_LOG(tag, format, ...)  ESP_LOGI(tag, format, ##__VA_ARGS__)
    #define DEBUG_LOGW(tag, format, ...) ESP_LOGW(tag, format, ##__VA_ARGS__)
    #define DEBUG_LOGE(tag, format, ...) ESP_LOGE(tag, format, ##__VA_ARGS__)
#else
    #define DEBUG_LOG(tag, format, ...)  do { (void)(tag); } while(0)
    #define DEBUG_LOGW(tag, format, ...) do { (void)(tag); } while(0)
    #define DEBUG_LOGE(tag, format, ...) do { (void)(tag); } while(0)
#endif

#endif // DEBUG_H
