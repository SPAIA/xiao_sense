#include <stdio.h>
#include "camera_interface.h"
#include "sdcard_interface.h"
#include "wifi_interface.h"
#include "file_upload.h"
#include "aht_interface.h"
#include "esp_log.h"
#include "upload_manager.h"

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
    if (initialize_sdcard() != ESP_OK)
    {
        ESP_LOGE("Main", "SD Card initialization failed!");
        return;
    }
    initialize_wifi();

    initialize_camera();
    init_file_upload_system();
}

void start_tasks()
{
    if (is_wifi_connected())
    {
        // WiFi already connected, start upload task immediately
        // Initialize upload manager first
        upload_manager_init(120);
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
}

void app_main(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d", reason);
    initialize_drivers();
    vTaskDelay(pdMS_TO_TICKS(1000));
    start_tasks();
    ESP_ERROR_CHECK(aht_init(AHT_I2C_SDA_GPIO, AHT_I2C_SCL_GPIO, AHT_I2C_PORT));

    // Create task to read sensor every 30 minutes (1800000 ms)
    ESP_ERROR_CHECK(aht_create_task(1800000, 0)); // Run on core 0
}