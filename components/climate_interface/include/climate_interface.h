#ifndef CLIMATE_INTERFACE_H
#define CLIMATE_INTERFACE_H

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "sdcard_interface.h"

#define SDA_GPIO 5
#define SCL_GPIO 6
#define I2C_PORT 0

void init_climate();
bool is_climate_sensor_available(void);

#endif // CAMERA_INTERFACE_H
