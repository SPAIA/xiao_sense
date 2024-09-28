
#ifndef WIFI_INTERFACE_H
#define WIFI_INTERFACE_H

#include "esp_event.h"

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

/**
 * @brief Initialize WiFi in station mode and connect to the specified AP
 *
 * This function initializes the WiFi driver, sets up event handlers,
 * and attempts to connect to the configured access point.
 */
void wifi_init_sta(void);

/**
 * @brief Event handler for WiFi and IP events
 *
 * @param arg User-provided event handler argument
 * @param event_base Base ID of the event to register the handler for
 * @param event_id ID of the event to register the handler for
 * @param event_data Event data
 */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data);

void initialize_wifi(void);

#endif // WIFI_INTERFACE_H