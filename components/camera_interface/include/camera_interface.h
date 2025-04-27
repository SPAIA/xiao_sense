#ifndef CAMERA_INTERFACE_H
#define CAMERA_INTERFACE_H

#include <time.h>
#include "esp_err.h"
#include "esp_camera.h"
#include "sensor.h"

// Initialize the camera
esp_err_t camera_manager_init(void);
esp_err_t initialize_camera(void); // Legacy function

// Capture a photo with the provided frame buffer
esp_err_t camera_manager_capture(camera_fb_t *fb, time_t ts);

// Run motion detection loop
// If motion is detected, returns true and sets *fb_out to the captured frame buffer
// Caller is responsible for returning the frame buffer with esp_camera_fb_return()
bool camera_manager_motion_loop(float thresh, time_t *stamp, camera_fb_t **fb_out);

// Create the camera task
void createCameraTask(void);

typedef struct
{
    uint16_t pid;
    uint32_t xclk_freq_hz;
    framesize_t max_frame_size;
} custom_sensor_info_t;

#endif // CAMERA_INTERFACE_H
