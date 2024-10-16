#include "bmp280.h"
#include "climate_interface.h"

void bmp280_test(void *pvParameters)
{

    bmp280_params_t params;
    bmp280_init_default_params(&params);
    bmp280_t dev;
    memset(&dev, 0, sizeof(bmp280_t));

    // Ensure SDA_GPIO and SCL_GPIO are defined correctly
    ESP_ERROR_CHECK(bmp280_init_desc(&dev, BMP280_I2C_ADDRESS_1, 0, SDA_GPIO, SCL_GPIO));
    ESP_ERROR_CHECK(bmp280_init(&dev, &params));

    bool bme280p = dev.id == BME280_CHIP_ID;
    printf("BMP280: found %s\n", bme280p ? "BME280" : "BMP280");

    float pressure, temperature, humidity;

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
        // Check for errors from bmp280_read_float
        if (bmp280_read_float(&dev, &temperature, &pressure, bme280p ? &humidity : NULL) != ESP_OK)
        {
            printf("Temperature/pressure reading failed\n");
            continue;
        }

        printf("Pressure: %.2f Pa, Temperature: %.2f C", pressure, temperature);
        sensor_data_t sensor_data;
        sensor_data.temperature = temperature;
        sensor_data.pressure = pressure;
        if (bme280p)
        {

            sensor_data.humidity = humidity;
            printf(", Humidity: %.2f\n", humidity);
        }
        else
            printf("\n");
        if (xQueueSend(sensor_data_queue, &sensor_data, pdMS_TO_TICKS(10)) != pdTRUE)
        {
            ESP_LOGE("climate", "Failed to send data to the queue");
        }
    }
}

void createClimateTask()
{
    // Increased stack size to ensure task stability
    xTaskCreatePinnedToCore(bmp280_test, "bmp280_test", configMINIMAL_STACK_SIZE * 6, NULL, 3, NULL, PRO_CPU_NUM);
}
void init_climate()
{
    ESP_ERROR_CHECK(i2cdev_init());
    createClimateTask();
}
