# WiFi Interface Component Guide

This document provides detailed information about the WiFi Interface component, which is responsible for managing network connectivity in the system.

## Overview

The WiFi Interface component:
- Manages WiFi connection and reconnection
- Provides status callbacks for connection state changes
- Enables other components to respond to connectivity changes
- Serves as the foundation for all network-dependent operations

## Architecture

The WiFi Interface provides a robust connectivity layer for the system:

```
┌─────────────────────────────────────────────────────────────┐
│                     WiFi Interface                           │
│                                                             │
│  ┌─────────────────┐      ┌─────────────────────────────┐  │
│  │ Configuration   │      │ Connection Management       │  │
│  │ - SSID          │      │ - Connect/disconnect        │  │
│  │ - Password      │      │ - Auto-reconnect            │  │
│  │ - Static IP     │      │ - Status monitoring         │  │
│  │   (optional)    │      │                             │  │
│  └─────────────────┘      └─────────────────────────────┘  │
│           │                            │                    │
│           │                            │                    │
│           ▼                            ▼                    │
│  ┌─────────────────┐      ┌─────────────────────────────┐  │
│  │ Event Handling  │      │ Callback System             │  │
│  │ - WiFi events   │      │ - Status change callbacks   │  │
│  │ - IP events     │      │ - Multiple callback support │  │
│  │ - Error handling│      │ - Thread-safe notification  │  │
│  │                 │      │                             │  │
│  └─────────────────┘      └─────────────────────────────┘  │
│                                        │                    │
└────────────────────────────────────────┼────────────────────┘
                                         │
                                         ▼
                            ┌─────────────────────────────┐
                            │ Application Components      │
                            │ - Upload Manager            │
                            │ - File Upload               │
                            │ - Other network-dependent   │
                            │   components                │
                            └─────────────────────────────┘
```

## Key Components

### Configuration Management
- Stores WiFi credentials (SSID, password)
- Supports optional static IP configuration
- Provides secure storage for sensitive information

### Connection Management
- Handles the WiFi connection process
- Implements automatic reconnection logic
- Monitors connection status

### Event Handling
- Processes ESP-IDF WiFi and IP events
- Manages error conditions and recovery
- Translates low-level events to high-level status

### Callback System
- Allows components to register for connection status changes
- Supports multiple callbacks for different components
- Provides thread-safe notification mechanism

## Implementation Details

### WiFi Initialization

The WiFi interface is initialized with configuration from a header file or flash storage:

```c
esp_err_t initialize_wifi(void)
{
    // Initialize the TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    
    // Create the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create the default WiFi station interface
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    
    // Initialize WiFi with default configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    
    // Configure WiFi station mode
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    // Set WiFi mode and configuration
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi initialization completed");
    
    // Connect to the AP
    ESP_LOGI(TAG, "Connecting to %s...", WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_connect());
    
    // Wait for connection (with timeout)
    int retry_count = 0;
    while (!is_wifi_connected() && retry_count < MAX_RETRY_COUNT) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry_count++;
        ESP_LOGI(TAG, "Waiting for WiFi connection... (%d/%d)", retry_count, MAX_RETRY_COUNT);
    }
    
    if (is_wifi_connected()) {
        ESP_LOGI(TAG, "Connected to WiFi network");
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "Failed to connect to WiFi network");
        return ESP_FAIL;
    }
}
```

### Event Handling

The WiFi interface processes events from the ESP-IDF event system:

```c
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi station started");
            esp_wifi_connect();
            break;
            
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "Connected to AP");
            break;
            
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disconnected = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "Disconnected from AP (reason: %d)", disconnected->reason);
            
            // Update connection status
            xSemaphoreTake(wifi_status_mutex, portMAX_DELAY);
            wifi_connected = false;
            xSemaphoreGive(wifi_status_mutex);
            
            // Notify callbacks about disconnection
            notify_wifi_status_callbacks(false);
            
            // Attempt to reconnect
            if (s_retry_num < MAX_RETRY_COUNT) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "Retrying connection (%d/%d)", s_retry_num, MAX_RETRY_COUNT);
            } else {
                ESP_LOGW(TAG, "Failed to connect after maximum retries");
                // Implement exponential backoff or other recovery strategy
                s_retry_num = 0;
                vTaskDelay(pdMS_TO_TICKS(10000)); // Wait 10 seconds before trying again
                esp_wifi_connect();
            }
            break;
        }
            
        default:
            ESP_LOGD(TAG, "Unhandled WiFi event: %ld", event_id);
            break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // Reset retry counter
        s_retry_num = 0;
        
        // Update connection status
        xSemaphoreTake(wifi_status_mutex, portMAX_DELAY);
        wifi_connected = true;
        xSemaphoreGive(wifi_status_mutex);
        
        // Notify callbacks about connection
        notify_wifi_status_callbacks(true);
    }
}
```

### Callback System

The WiFi interface allows other components to register for status change notifications:

```c
// Callback type definition
typedef void (*wifi_status_callback_t)(bool connected);

// Array of registered callbacks
static wifi_status_callback_t status_callbacks[MAX_CALLBACKS];
static int num_callbacks = 0;
static SemaphoreHandle_t callback_mutex = NULL;

// Register a callback
esp_err_t register_wifi_status_callback(wifi_status_callback_t callback)
{
    if (!callback_mutex) {
        callback_mutex = xSemaphoreCreateMutex();
        if (!callback_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    
    if (xSemaphoreTake(callback_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (num_callbacks < MAX_CALLBACKS) {
            status_callbacks[num_callbacks++] = callback;
            xSemaphoreGive(callback_mutex);
            
            // If already connected, immediately notify the new callback
            if (is_wifi_connected()) {
                callback(true);
            }
            
            return ESP_OK;
        }
        xSemaphoreGive(callback_mutex);
        return ESP_ERR_NO_MEM;
    }
    
    return ESP_ERR_TIMEOUT;
}

// Notify all registered callbacks
static void notify_wifi_status_callbacks(bool connected)
{
    if (xSemaphoreTake(callback_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < num_callbacks; i++) {
            if (status_callbacks[i]) {
                status_callbacks[i](connected);
            }
        }
        xSemaphoreGive(callback_mutex);
    }
}
```

## Integration with Other Components

### Main Application
The main application registers a callback to respond to WiFi status changes:

```c
void app_main(void)
{
    initialize_drivers();
    register_wifi_status_callback(on_wifi_status_change);
    if (is_wifi_connected())
    {
        ESP_LOGI(TAG, "WiFi already connected - manually triggering callback");
        on_wifi_status_change(true);
    }
}

void on_wifi_status_change(bool connected)
{
    ESP_LOGI(TAG, "WiFi status callback triggered - connected: %d", connected);

    if (connected && !upload_task_started)
    {
        ESP_LOGI(TAG, "WiFi connected - initializing subsystems");

        // Now safe to bring up power-hungry and blocking tasks
        init_file_upload_system();

        upload_folder();
        upload_task_started = true;

        create_data_log_queue();
        createCameraTask();

        ESP_ERROR_CHECK(aht_init(AHT_I2C_SDA_GPIO, AHT_I2C_SCL_GPIO, AHT_I2C_PORT));
        ESP_ERROR_CHECK(aht_create_task(10000, 0));
        upload_manager_init(60);
    }
    else if (!connected && upload_task_started)
    {
        ESP_LOGW(TAG, "WiFi lost - not restarting tasks, waiting for reconnect");
        upload_task_started = false;
    }
}
```

### Upload Manager and File Upload
These components check WiFi status before attempting network operations:

```c
// Example from file_upload.c
if (stat(request.filepath, &st) == 0 && is_wifi_connected())
{
    ESP_LOGI(TAG, "File exists, starting upload: %s", request.filepath);
    esp_err_t result = upload_file_to_https(request.filepath, request.url, CONFIG_SPAIA_DEVICE_ID);
    // ...
}
```

## Configuration

### WiFi Credentials
WiFi credentials are typically stored in a header file or secure storage:

```c
// Example from wifi_config.h
#define WIFI_SSID "YourNetworkName"
#define WIFI_PASSWORD "YourNetworkPassword"
```

### Connection Parameters
Various parameters control the connection behavior:

```c
#define MAX_RETRY_COUNT 10        // Maximum connection retry attempts
#define RECONNECT_INTERVAL 5000   // Milliseconds between reconnection attempts
#define MAX_CALLBACKS 5           // Maximum number of status callbacks
```

## Performance Considerations

### Power Management
- WiFi is a significant power consumer
- Consider implementing power-saving modes for battery operation
- Balance between connectivity needs and power consumption

### Memory Usage
- WiFi stack requires significant memory
- Ensure sufficient heap is available for WiFi operations
- Monitor for memory leaks in long-running applications

### Reliability
- Implement robust reconnection strategies
- Handle edge cases like weak signals or intermittent connectivity
- Consider fallback mechanisms for critical operations

## Troubleshooting

### Common Issues

1. **Connection Failures**
   - Verify credentials are correct
   - Check signal strength and AP availability
   - Ensure the device is within range of the access point

2. **Intermittent Connectivity**
   - Implement more aggressive reconnection strategies
   - Check for interference or congestion
   - Consider using a more reliable frequency band (5GHz vs 2.4GHz)

3. **High Power Consumption**
   - Implement power-saving modes
   - Reduce connection check frequency
   - Use modem sleep when appropriate

4. **Memory Issues**
   - Increase heap size allocation
   - Check for memory leaks in WiFi event handling
   - Optimize buffer sizes for network operations

## Extending the WiFi Interface

### Possible Enhancements

1. **Multiple Network Support**
   - Implement connection to alternative networks
   - Create a priority-based network selection system

2. **Advanced Authentication**
   - Support for enterprise authentication methods
   - Certificate-based authentication

3. **Captive Portal Detection**
   - Detect and handle captive portals
   - Implement automatic portal navigation

4. **Mesh Networking**
   - Extend to support ESP-MESH for wider coverage
   - Implement node-to-node communication
