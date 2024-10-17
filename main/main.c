#include <stdio.h>
#include "camera_interface.h"
#include "sdcard_interface.h"
#include "wifi_interface.h"
#include "file_upload.h"
#include "climate_interface.h"

void initialize_drivers()
{
    initialize_wifi();
    vTaskDelay(pdMS_TO_TICKS(500));
    initialize_sdcard();
    initialize_camera();
    init_file_upload_system();
}

void start_tasks()
{
    // createCameraTask();
    create_data_log_queue();
    // Read CSV files from SD card
    char file_list[MAX_FILES][MAX_FILE_PATH];
    int file_count = 0;

    char full_path[256]; // Adjust size as needed
    snprintf(full_path, sizeof(full_path), "%s/spaia", MOUNT_POINT);
    esp_err_t result = sdcard_read_csv_files(full_path, file_list, &file_count, MAX_FILES);

    if (result == ESP_OK)
    {
        ESP_LOGI("MAIN", "Found %d CSV files", file_count);
        // Queue files for upload
        for (int i = 0; i < file_count; i++)
        {
            queue_file_upload(file_list[i], "https://device.spaia.earth/upload");
        }
    }
    else
    {
        ESP_LOGE("MAIN", "Failed to read CSV files from SD card");
    }
}

void app_main(void)
{
    initialize_drivers();
    vTaskDelay(pdMS_TO_TICKS(500));
    start_tasks();
    init_climate();
}