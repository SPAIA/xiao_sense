#ifndef AHT_INTERFACE_H
#define AHT_INTERFACE_H

#include "esp_err.h"
#include "driver/i2c.h"

// Default I2C settings
#define AHT_I2C_PORT I2C_NUM_0
#define AHT_I2C_SDA_GPIO 5 // Default SDA pin, adjust if needed
#define AHT_I2C_SCL_GPIO 6 // Default SCL pin, adjust if needed
#define AHT_I2C_ADDR 0x38

// AHT20 commands
#define AHT_CMD_INIT 0xBE
#define AHT_CMD_TRIGGER 0xAC
#define AHT_CMD_RESET 0xBA
#define AHT_CMD_STATUS 0x71
#define AHT_DATA_BYTES 7

// I2C master port
#define I2C_MASTER_PORT AHT_I2C_PORT

/**
 * @brief Initialize the AHT20 sensor
 *
 * @param sda_gpio SDA GPIO pin number
 * @param scl_gpio SCL GPIO pin number
 * @param i2c_port I2C port number
 *
 * @return ESP_OK if successful, otherwise an error code
 */
esp_err_t aht_init(gpio_num_t sda_gpio, gpio_num_t scl_gpio, i2c_port_t i2c_port);

/**
 * @brief Read temperature and humidity values from AHT20
 *
 * @param temperature Pointer to store temperature value (in °C)
 * @param humidity Pointer to store humidity value (in %)
 *
 * @return ESP_OK if successful, otherwise an error code
 */
esp_err_t aht_read_data(float *temperature, float *humidity);

/**
 * @brief Create a task to read AHT20 sensor periodically
 *
 * @param interval_ms Interval in milliseconds between readings
 * @param core_id CPU core to run the task on (0 or 1)
 *
 * @return ESP_OK if successful, otherwise an error code
 */
esp_err_t aht_create_task(uint32_t interval_ms, int core_id);

/**
 * @brief Get the last temperature reading
 *
 * @return Last temperature reading in °C
 */
float aht_get_temperature(void);

/**
 * @brief Get the last humidity reading
 *
 * @return Last humidity reading in %
 */
float aht_get_humidity(void);

#endif /* AHT_INTERFACE_H */
