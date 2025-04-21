#ifndef FILE_UPLOAD_H
#define FILE_UPLOAD_H

#include "esp_err.h"

/**
 * @brief Initialize the file upload system
 *
 * This function creates the upload queue and starts the file upload task.
 */
void init_file_upload_system(void);

/**
 * @brief Queue a file for upload
 *
 * @param filepath The path to the file to be uploaded
 * @param url The URL to upload the file to
 * @return esp_err_t ESP_OK if the upload was queued successfully, ESP_FAIL otherwise
 */
esp_err_t queue_file_upload(const char *filepath, const char *url);

/**
 * @brief Upload all files in the spaia directory
 *
 * This function scans the spaia directory on the SD card and queues all files for upload.
 *
 * @return esp_err_t ESP_OK if at least one file was queued, ESP_FAIL otherwise
 */
esp_err_t upload_all_files(void);

#endif // FILE_UPLOAD_H
