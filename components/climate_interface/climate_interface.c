#include "bmp280.h"
#include "climate_interface.h"
#include <esp_err.h>
#include <esp_log.h>

#define READING_INTERVAL_MS (30 * 60 * 1000) // 30 minutes in milliseconds
#define TAG "climate"

static bool sensor_available = false;

static esp_err_t check_sensor_available(bmp280_t *dev, bmp280_params_t *params)
{
    // First check if we can communicate with the sensor
    esp_err_t ret = bmp280_init_desc(dev, BMP280_I2C_ADDRESS_1, 0, SDA_GPIO, SCL_GPIO);
    if (ret == ESP_ERR_TIMEOUT)
    {
        ESP_LOGW(TAG, "I2C timeout while trying to communicate with sensor");
        return ESP_ERR_TIMEOUT;
    }
    else if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init sensor descriptor: %d", ret);
        return ret;
    }

    // Try to initialize the sensor
    ret = bmp280_init(dev, params);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init sensor: %d", ret);
        // Clean up the I2C descriptor
        bmp280_free_desc(dev);
        return ret;
    }

    return ESP_OK;
}

void bmp280_test(void *pvParameters)
{
    bmp280_params_t params;
    bmp280_init_default_params(&params);
    bmp280_t dev;
    memset(&dev, 0, sizeof(bmp280_t));

    // Try to initialize the sensor
    esp_err_t ret = check_sensor_available(&dev, &params);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "No BMP/BME sensor detected or initialization failed");
        sensor_available = false;
        vTaskDelete(NULL);
        return;
    }

    sensor_available = true;
    bool bme280p = dev.id == BME280_CHIP_ID;
    ESP_LOGI(TAG, "Found %s sensor", bme280p ? "BME280" : "BMP280");

    float pressure, temperature, humidity;

    while (1)
    {
        // Wake up the sensor
        params.mode = BMP280_MODE_FORCED;
        ret = bmp280_init(&dev, &params);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to set forced mode");
            vTaskDelay(pdMS_TO_TICKS(READING_INTERVAL_MS));
            continue;
        }

        // Wait for the measurement to complete
        vTaskDelay(pdMS_TO_TICKS(10));

        // Take a reading
        ret = bmp280_read_float(&dev, &temperature, &pressure, bme280p ? &humidity : NULL);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Temperature/pressure reading failed: %d", ret);
        }
        else
        {
            ESP_LOGI(TAG, "Pressure: %.2f Pa, Temperature: %.2f C%s%s%.2f",
                     pressure, temperature,
                     bme280p ? ", Humidity: " : "",
                     bme280p ? "%" : "",
                     bme280p ? humidity : 0.0f);

            sensor_data_t sensor_data = {
                .timestamp = time(NULL),
                .temperature = temperature,
                .pressure = pressure,
                .humidity = bme280p ? humidity : 0.0f};

            if (xQueueSend(sensor_data_queue, &sensor_data, pdMS_TO_TICKS(10)) != pdTRUE)
            {
                ESP_LOGE(TAG, "Failed to send data to the queue");
            }
        }

        // Wait for the next reading interval
        vTaskDelay(pdMS_TO_TICKS(READING_INTERVAL_MS));
    }

    // This point should never be reached, but cleanup just in case
    bmp280_free_desc(&dev);
}

bool is_climate_sensor_available(void)
{
    return sensor_available;
}

void createClimateTask(void)
{
    xTaskCreatePinnedToCore(bmp280_test, "bmp280_test", configMINIMAL_STACK_SIZE * 8, NULL, 3, NULL, PRO_CPU_NUM);
}

void init_climate(void)
{
    esp_err_t ret = i2cdev_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize I2C: %d", ret);
        return;
    }
    sensor_available = false;
    createClimateTask();
}