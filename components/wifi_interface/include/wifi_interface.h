#ifndef WIFI_INTERFACE_H
#define WIFI_INTERFACE_H

#include <stdbool.h>
#include "esp_err.h"

// Initialize the WiFi system
void initialize_wifi(void);

// Get current WiFi connection status
bool is_wifi_connected(void);

// Register a callback for WiFi status changes
typedef void (*wifi_status_callback_t)(bool connected);
esp_err_t register_wifi_status_callback(wifi_status_callback_t callback);

#define MAXIMUM_RETRY 5

#endif // WIFI_INTERFACE_H