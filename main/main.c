#include <stdio.h>
#include "camera_interface.h"
#include "sdcard_interface.h"
#include "wifi_interface.h"
#include "file_upload.h"
#include "climate_interface.h"
#include "esp_log.h"

static bool upload_task_started = false;

#define TAG "main"

void on_wifi_status_change(bool connected)
{
    if (connected && !upload_task_started)
    {
        ESP_LOGI(TAG, "WiFi connected - starting upload task");
        upload_folder();
        upload_task_started = true;
    }
    else if (!connected && upload_task_started)
    {
        ESP_LOGI(TAG, "WiFi disconnected - upload task will be started when WiFi reconnects");
        upload_task_started = false;
    }
}

void initialize_drivers()
{
    initialize_wifi();
    initialize_sdcard();
    initialize_camera();
    init_file_upload_system();
}

void start_tasks()
{
    if (is_wifi_connected())
    {
        // WiFi already connected, start upload task immediately
        upload_folder();
        upload_task_started = true;
    }
    else
    {
        // Not connected, register callback to start upload task when WiFi connects
        ESP_LOGI(TAG, "WiFi not connected - registering callback for when WiFi connects");
        register_wifi_status_callback(on_wifi_status_change);
    }
    create_data_log_queue();
    createCameraTask();

    // init_climate();
}

void app_main(void)
{
    initialize_drivers();
    start_tasks();
}