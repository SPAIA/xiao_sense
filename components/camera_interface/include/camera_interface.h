#ifndef CAMERA_INTERFACE_H
#define CAMERA_INTERFACE_H

#include <time.h>
#include "esp_err.h"
#include "esp_camera.h"
#include "sensor.h"

esp_err_t initialize_camera(void);
esp_err_t takeHighResPhoto(time_t timestamp);

void createCameraTask(void);

typedef struct
{
    uint16_t pid;
    uint32_t xclk_freq_hz;
    framesize_t max_frame_size;
} custom_sensor_info_t;

#endif // CAMERA_INTERFACE_H