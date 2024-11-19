#include <stdio.h>
#include "camera_interface.h"
#include "sdcard_interface.h"
#include "wifi_interface.h"
#include "file_upload.h"
#include "climate_interface.h"

void initialize_drivers()
{
    initialize_wifi();
    initialize_sdcard();
    initialize_camera();
    init_file_upload_system();
}

void start_tasks()
{
    upload_folder();
    create_data_log_queue();
    createCameraTask();

    // init_climate();
}

void app_main(void)
{
    initialize_drivers();
    start_tasks();
}