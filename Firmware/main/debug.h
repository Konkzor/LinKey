#ifndef DEBUG_H
#define DEBUG_H

#include "esp_log.h"

// Debug logging macro - controlled by Kconfig
#ifdef CONFIG_LINKY_DEBUG_LOGS
    #define DEBUG_LOG(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
#else
    #define DEBUG_LOG(tag, format, ...) do {} while(0)
#endif

#endif // DEBUG_H
