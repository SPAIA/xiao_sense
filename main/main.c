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
    ESP_LOGI(TAG, "WiFi status callback triggered - connected: %d", connected);

    if (connected && !upload_task_started)
    {
        ESP_LOGI(TAG, "WiFi connected - initializing subsystems");

        // Now safe to bring up power-hungry and blocking tasks
        initialize_camera();
        init_file_upload_system();

        // Initialize upload manager first
        upload_manager_init(0);

        // Then upload existing files
        upload_all_files();
        upload_task_started = true;

        create_data_log_queue();
        createCameraTask();

        ESP_ERROR_CHECK(aht_init(AHT_I2C_SDA_GPIO, AHT_I2C_SCL_GPIO, AHT_I2C_PORT));
        ESP_ERROR_CHECK(aht_create_task(3600000, 0)); // Update every hour (3600000 ms)
    }
    else if (!connected && upload_task_started)
    {
        ESP_LOGW(TAG, "WiFi lost - not restarting tasks, waiting for reconnect");
        upload_task_started = false;
    }
}

void initialize_drivers()
{
    // Only init SD and WiFi here
    if (initialize_sdcard() != ESP_OK)
    {
        ESP_LOGE("Main", "SD Card initialization failed!");
        return;
    }

    ESP_LOGI(TAG, "Initializing WiFi...");
    initialize_wifi(); // this includes waiting for connection
}

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
