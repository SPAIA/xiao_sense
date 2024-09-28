#include <stdio.h>
#include "camera_interface.h"
#include "sdcard_interface.h"
#include "wifi_interface.h"
#include "file_upload.h"

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
    createCameraTask();
}

void app_main(void)
{
    initialize_drivers();
    vTaskDelay(pdMS_TO_TICKS(500));
    start_tasks();
}