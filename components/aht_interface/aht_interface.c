#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "aht_interface.h"

static const char *TAG = "aht_interface";

// Global variables
static i2c_dev_t aht_dev;
static SemaphoreHandle_t aht_data_mutex = NULL;
static TaskHandle_t aht_task_handle = NULL;
static float last_temperature = 0.0f;
static float last_humidity = 0.0f;
static bool is_initialized = false;

// Read sensor data
static esp_err_t read_aht_sensor(float *temp, float *hum)
{
    esp_err_t res;

    // Take mutex
    if ((res = i2c_dev_take_mutex(&aht_dev)) != ESP_OK)
    {
        return res;
    }

    // Trigger measurement
    uint8_t trigger_cmd[3] = {AHT_CMD_TRIGGER, 0x33, 0x00};
    res = i2c_dev_write(&aht_dev, NULL, 0, trigger_cmd, sizeof(trigger_cmd));
    if (res != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not trigger measurement: %d (%s)", res, esp_err_to_name(res));
        i2c_dev_give_mutex(&aht_dev);
        return res;
    }

    // Wait for measurement to complete (at least 80ms)
    vTaskDelay(pdMS_TO_TICKS(80));

    // Read the measurement data
    uint8_t data[AHT_DATA_BYTES];
    res = i2c_dev_read(&aht_dev, NULL, 0, data, sizeof(data));

    // Release mutex
    i2c_dev_give_mutex(&aht_dev);

    if (res != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not read data: %d (%s)", res, esp_err_to_name(res));
        return res;
    }

    // Check if data is valid (bit 7 of status byte should be 0)
    if ((data[0] & 0x80) != 0)
    {
        ESP_LOGE(TAG, "Sensor busy or data not valid (status: 0x%02x)", data[0]);
        return ESP_ERR_INVALID_STATE;
    }

    // Calculate humidity (20 bits)
    uint32_t humidity_raw = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    *hum = (float)humidity_raw * 100 / 1048576.0;

    // Calculate temperature (20 bits)
    uint32_t temp_raw = ((uint32_t)(data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];
    *temp = (float)temp_raw * 200 / 1048576.0 - 50;

    return ESP_OK;
}

// Initialize the AHT20 sensor
static esp_err_t init_aht_sensor(void)
{
    // Take mutex
    esp_err_t res = i2c_dev_take_mutex(&aht_dev);
    if (res != ESP_OK)
    {
        return res;
    }

    // Initialize the AHT20 sensor
    uint8_t init_cmd[3] = {AHT_CMD_INIT, 0x08, 0x00};
    res = i2c_dev_write(&aht_dev, NULL, 0, init_cmd, sizeof(init_cmd));

    // Release mutex
    i2c_dev_give_mutex(&aht_dev);

    if (res != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not initialize AHT20 sensor: %d (%s)", res, esp_err_to_name(res));
        return res;
    }

    // Wait for initialization to complete
    vTaskDelay(pdMS_TO_TICKS(40));

    return ESP_OK;
}

// AHT20 reading task
static void aht_reading_task(void *pvParameters)
{
    uint32_t interval_ms = *((uint32_t *)pvParameters);
    free(pvParameters); // Free the allocated memory for interval

    ESP_LOGI(TAG, "AHT20 sensor task started, reading every %lu ms", interval_ms);

    while (1)
    {
        float temp, hum;

        // Read sensor data
        if (read_aht_sensor(&temp, &hum) == ESP_OK)
        {
            // Update the global values with mutex protection
            if (xSemaphoreTake(aht_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                last_temperature = temp;
                last_humidity = hum;
                xSemaphoreGive(aht_data_mutex);
            }

            ESP_LOGI(TAG, "Temperature: %.2f°C, Humidity: %.2f%%", temp, hum);
        }

        // Wait for the next reading interval
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

// Public API implementations
esp_err_t aht_init(int sda_gpio, int scl_gpio, i2c_port_t i2c_port)
{
    if (is_initialized)
    {
        ESP_LOGW(TAG, "AHT sensor already initialized");
        return ESP_OK;
    }

    // Create mutex for data protection
    aht_data_mutex = xSemaphoreCreateMutex();
    if (!aht_data_mutex)
    {
        ESP_LOGE(TAG, "Failed to create data mutex");
        return ESP_FAIL;
    }

    // Initialize i2cdev library if not already done
    esp_err_t ret = i2cdev_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Failed to initialize I2C: %d (%s)", ret, esp_err_to_name(ret));
        return ret;
    }

    // Configure the AHT20 device
    memset(&aht_dev, 0, sizeof(i2c_dev_t));
    aht_dev.port = i2c_port;
    aht_dev.addr = AHT_I2C_ADDR;
    aht_dev.cfg.sda_io_num = sda_gpio;
    aht_dev.cfg.scl_io_num = scl_gpio;
    aht_dev.cfg.master.clk_speed = 100000;
    aht_dev.cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
    aht_dev.cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;

    // Create mutex for the device
    ret = i2c_dev_create_mutex(&aht_dev);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create device mutex: %d (%s)", ret, esp_err_to_name(ret));
        return ret;
    }

    // Initialize the AHT20 sensor
    ret = init_aht_sensor();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize sensor: %d (%s)", ret, esp_err_to_name(ret));
        return ret;
    }

    is_initialized = true;
    ESP_LOGI(TAG, "AHT sensor initialized successfully");
    return ESP_OK;
}

esp_err_t aht_read_data(float *temperature, float *humidity)
{
    if (!is_initialized)
    {
        ESP_LOGE(TAG, "AHT sensor not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    return read_aht_sensor(temperature, humidity);
}

esp_err_t aht_create_task(uint32_t interval_ms, int core_id)
{
    if (!is_initialized)
    {
        ESP_LOGE(TAG, "AHT sensor not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (aht_task_handle != NULL)
    {
        ESP_LOGW(TAG, "AHT sensor task already running");
        return ESP_OK;
    }

    // Allocate memory for the interval parameter (will be freed by the task)
    uint32_t *interval_param = (uint32_t *)malloc(sizeof(uint32_t));
    if (!interval_param)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for task parameter");
        return ESP_ERR_NO_MEM;
    }
    *interval_param = interval_ms;

    // Create the AHT20 reading task pinned to specified core
    BaseType_t result = xTaskCreatePinnedToCore(
        aht_reading_task, // Task function
        "aht_task",       // Task name
        4096,             // Stack size (in words)
        interval_param,   // Task parameters
        5,                // Task priority
        &aht_task_handle, // Task handle
        core_id           // Core ID
    );

    if (result != pdPASS || aht_task_handle == NULL)
    {
        ESP_LOGE(TAG, "Failed to create AHT task");
        free(interval_param);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AHT task created successfully");
    return ESP_OK;
}

float aht_get_temperature(void)
{
    float temp = 0.0f;

    if (is_initialized && xSemaphoreTake(aht_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        temp = last_temperature;
        xSemaphoreGive(aht_data_mutex);
    }

    return temp;
}

float aht_get_humidity(void)
{
    float hum = 0.0f;

    if (is_initialized && xSemaphoreTake(aht_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        hum = last_humidity;
        xSemaphoreGive(aht_data_mutex);
    }

    return hum;
}