#ifndef SDCARD_INTERFACE_H
#define SDCARD_INTERFACE_H

#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define MAX_FILE_PATH 256
#define MAX_FILES 20
#define MOUNT_POINT "/sd"

// Structure to hold sensor data
typedef struct
{
    time_t timestamp;
    float temperature;
    float humidity;
    float pressure;
    char *bboxes;
    bool owns_bboxes;
} sensor_data_t;

// Declare the queue handle as an extern variable
extern QueueHandle_t sensor_data_queue;

// Function prototypes
void initialize_sdcard(void);
void deinitialise_sdcard(void);
esp_err_t saveJpegToSdcard(camera_fb_t *fb, time_t timestamp);
void create_data_log_queue(void);
void append_data_to_csv(time_t timestamp, float temperature, float humidity, float pressure, const char *bboxes);
void log_sensor_data_task(void *pvParameters);
void upload_folder();

#endif // SDCARD_INTERFACE_H