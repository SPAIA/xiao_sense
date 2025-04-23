// aht_interface.c — AHT20 driver using ESP‑IDF v5.4+ new I²C master API
// SPDX‑License‑Identifier: MIT

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h" // new API
#include "aht_interface.h"
#include "esp_check.h"

/* -------------------------------------------------------------------------- */
/*  AHT20 protocol constants                                                  */
/* -------------------------------------------------------------------------- */
#define AHT20_I2C_ADDR 0x38
#define AHT20_CMD_TRIGGER 0xAC // trigger measurement, hold master
#define AHT20_CMD_INIT 0xBE    // initialization/ calibration
#define AHT_BYTES_MEASUREMENT 6

/* -------------------------------------------------------------------------- */
/*  Globals                                                                   */
/* -------------------------------------------------------------------------- */
static const char *TAG = "aht20-ng";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static SemaphoreHandle_t s_data_mtx = NULL;
static TaskHandle_t s_task = NULL;

static float s_last_temp = 0; // cached values published to callers
static float s_last_hum = 0;
static bool s_init_done = false;

/* -------------------------------------------------------------------------- */
/*  Low‑level helpers                                                         */
/* -------------------------------------------------------------------------- */
static inline esp_err_t aht_write(const uint8_t *data, size_t len)
{
    return i2c_master_transmit(s_dev, data, len, -1 /* block for ever */);
}

static inline esp_err_t aht_read(uint8_t *data, size_t len)
{
    return i2c_master_receive(s_dev, data, len, -1);
}

static esp_err_t aht_bus_init(i2c_port_t port, gpio_num_t sda, gpio_num_t scl)
{
    /* Configure and create the I²C bus */
    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_bus), TAG, "bus create failed");

    /* Attach the AHT20 as a device on that bus */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AHT20_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    // Use i2c_master_bus_add_device instead of i2c_master_create_device
    return i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
}

static esp_err_t aht20_init_chip(void)
{
    uint8_t cmd[3] = {AHT20_CMD_INIT, 0x08, 0x00};
    ESP_RETURN_ON_ERROR(aht_write(cmd, sizeof(cmd)), TAG, "chip init write");
    vTaskDelay(pdMS_TO_TICKS(40)); // ≥40 ms according to datasheet
    return ESP_OK;
}

static esp_err_t aht20_sample(float *temp, float *hum)
{
    uint8_t trig[3] = {AHT20_CMD_TRIGGER, 0x33, 0x00};
    ESP_RETURN_ON_ERROR(aht_write(trig, sizeof(trig)), TAG, "trigger");
    vTaskDelay(pdMS_TO_TICKS(80)); // t_meas ≈ 80 ms

    uint8_t raw[AHT_BYTES_MEASUREMENT];
    ESP_RETURN_ON_ERROR(aht_read(raw, sizeof(raw)), TAG, "read");

    if (raw[0] & 0x80)
        return ESP_ERR_INVALID_STATE; // busy flag

    uint32_t h = (raw[1] << 12) | (raw[2] << 4) | (raw[3] >> 4);
    uint32_t t = ((raw[3] & 0x0F) << 16) | (raw[4] << 8) | raw[5];

    *hum = h * 100.0f / 1048576.0f;
    *temp = t * 200.0f / 1048576.0f - 50.0f;
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  FreeRTOS background task                                                  */
/* -------------------------------------------------------------------------- */
static void aht_task(void *arg)
{
    uint32_t interval_ms = (uint32_t)(uintptr_t)arg;
    ESP_LOGI(TAG, "started, interval %lu ms", (unsigned long)interval_ms);

    while (1)
    {
        float t, h;
        if (aht20_sample(&t, &h) == ESP_OK)
        {
            if (xSemaphoreTake(s_data_mtx, portMAX_DELAY) == pdTRUE)
            {
                s_last_temp = t;
                s_last_hum = h;
                xSemaphoreGive(s_data_mtx);
            }
            ESP_LOGI(TAG, "T=%.2f °C, RH=%.2f %%", t, h);
        }
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */
esp_err_t aht_init(gpio_num_t sda, gpio_num_t scl, i2c_port_t port)
{
    if (s_init_done)
        return ESP_OK;

    s_data_mtx = xSemaphoreCreateMutex();
    if (!s_data_mtx)
        return ESP_ERR_NO_MEM;

    ESP_RETURN_ON_ERROR(aht_bus_init(port, sda, scl), TAG, "bus init");
    ESP_RETURN_ON_ERROR(aht20_init_chip(), TAG, "chip init");

    s_init_done = true;
    return ESP_OK;
}

esp_err_t aht_create_task(uint32_t interval_ms, BaseType_t core)
{
    if (!s_init_done)
        return ESP_ERR_INVALID_STATE;
    if (s_task)
        return ESP_OK;

    return xTaskCreatePinnedToCore(aht_task, "aht", 4096, (void *)(uintptr_t)interval_ms,
                                   5, &s_task, core) == pdPASS
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t aht_read_data(float *t, float *h)
{
    if (!s_init_done)
        return ESP_ERR_INVALID_STATE;
    return aht20_sample(t, h);
}

float aht_get_temperature(void)
{
    float v = 0;
    if (s_init_done && xSemaphoreTake(s_data_mtx, 0) == pdTRUE)
    {
        v = s_last_temp;
        xSemaphoreGive(s_data_mtx);
    }
    return v;
}

float aht_get_humidity(void)
{
    float v = 0;
    if (s_init_done && xSemaphoreTake(s_data_mtx, 0) == pdTRUE)
    {
        v = s_last_hum;
        xSemaphoreGive(s_data_mtx);
    }
    return v;
}

/* -------------------------------------------------------------------------- */
/*  Cleanup (optional)                                                        */
/* -------------------------------------------------------------------------- */
void aht_deinit(void)
{
    if (!s_init_done)
        return;

    if (s_task)
    {
        vTaskDelete(s_task);
        s_task = NULL;
    }

    if (s_dev)
    {
        // Use i2c_master_bus_rm_device instead of i2c_master_del_device
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus)
    {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    if (s_data_mtx)
    {
        vSemaphoreDelete(s_data_mtx);
        s_data_mtx = NULL;
    }

    s_init_done = false;
}