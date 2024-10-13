#ifndef SDCARD_INTERFACE_H
#define SDCARD_INTERFACE_H

#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Structure to hold sensor data
typedef struct
{
    float temperature;
    float humidity;
    float pressure;
    char bboxes[100]; // Adjust size as needed
} sensor_data_t;

// Declare the queue handle as an extern variable
extern QueueHandle_t sensor_data_queue;

// Function prototypes
void initialize_sdcard(void);
void deinitialise_sdcard(void);
void saveJpegToSdcard(camera_fb_t *fb);
void create_data_log_queue(void);
void append_data_to_csv(float temperature, float humidity, float pressure, const char *bboxes);
void log_sensor_data_task(void *pvParameters);

#endif // SDCARD_INTERFACE_H