#ifndef UPLOAD_MANAGER_H
#define UPLOAD_MANAGER_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialize the upload manager
     *
     * @param upload_interval_seconds Interval in seconds between uploads (0 = real-time)
     * @return esp_err_t ESP_OK on success, otherwise an error code
     */
    esp_err_t upload_manager_init(uint32_t upload_interval_seconds);

    /**
     * @brief Set the upload interval
     *
     * @param upload_interval_seconds Interval in seconds between uploads (0 = real-time)
     * @return esp_err_t ESP_OK on success, otherwise an error code
     */
    esp_err_t upload_manager_set_interval(uint32_t upload_interval_seconds);

    /**
     * @brief Request an immediate upload
     *
     * @return esp_err_t ESP_OK on success, otherwise an error code
     */
    esp_err_t upload_manager_upload_now(void);

    /**
     * @brief Notify the upload manager that a new file has been created
     *
     * @param filename Path to the file that has been created or updated
     * @return esp_err_t ESP_OK on success, otherwise an error code
     */
    esp_err_t upload_manager_notify_new_file(const char *filename);

    /**
     * @brief Modified version of append_data_to_csv that uses the upload manager
     *
     * @param timestamp Unix timestamp
     * @param temperature Temperature value
     * @param humidity Humidity value
     * @param pressure Pressure value
     * @param bboxes Bounding boxes string (can be NULL)
     * @return esp_err_t ESP_OK on success, otherwise an error code
     */
    esp_err_t modified_append_data_to_csv(time_t timestamp, float temperature, float humidity, float pressure, const char *bboxes);

#ifdef __cplusplus
}
#endif

#endif /* UPLOAD_MANAGER_H */