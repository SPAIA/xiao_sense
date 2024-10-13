#ifndef CLIMATE_INTERFACE_H
#define CLIMATE_INTERFACE_H

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#define SDA_GPIO 5
#define SCL_GPIO 6
#define I2C_PORT 0

static const char *TAG = "bmp280_example";

void createClimatTask();
void init_climate();
void bmp280_test(void *pvParameters);
#endif // CAMERA_INTERFACE_H
