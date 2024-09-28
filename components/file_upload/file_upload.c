#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "sdcard_interface.h"
#include "wifi_interface.h"

#define MAX_FILE_PATH 64
#define MAX_URL_LENGTH 256

typedef struct
{
    char filepath[MAX_FILE_PATH];
    char url[MAX_URL_LENGTH];
} UploadRequest;

static const char *TAG = "file_upload";
static QueueHandle_t upload_queue;

esp_err_t upload_file_to_https(const char *filepath, const char *url)
{
    FILE *f = fopen(filepath, "r");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", filepath);
        return ESP_FAIL;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .cert_pem = NULL, // Add your server's root certificate here if needed
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    char buff[1024];
    size_t read_bytes;
    while ((read_bytes = fread(buff, 1, sizeof(buff), f)) > 0)
    {
        esp_http_client_write(client, buff, read_bytes);
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "File uploaded successfully: %s", filepath);
    }
    else
    {
        ESP_LOGE(TAG, "File upload failed: %s", esp_err_to_name(err));
    }

    fclose(f);
    esp_http_client_cleanup(client);
    return err;
}

void file_upload_task(void *pvParameters)
{
    UploadRequest request;
    struct stat st;

    for (;;)
    {
        if (xQueueReceive(upload_queue, &request, portMAX_DELAY) == pdTRUE)
        {
            if (stat(request.filepath, &st) == 0)
            {
                ESP_LOGI(TAG, "File exists, starting upload: %s", request.filepath);
                esp_err_t result = upload_file_to_https(request.filepath, request.url);
                if (result == ESP_OK)
                {
                    ESP_LOGI(TAG, "Upload completed successfully");
                    // Optionally, delete the file after successful upload
                    // unlink(request.filepath);
                }
                else
                {
                    ESP_LOGE(TAG, "Upload failed");
                }
            }
            else
            {
                ESP_LOGE(TAG, "File does not exist: %s", request.filepath);
            }
        }
    }
}

void init_file_upload_system()
{
    upload_queue = xQueueCreate(10, sizeof(UploadRequest));
    xTaskCreate(file_upload_task, "file_upload_task", 4096, NULL, 5, NULL);
}

esp_err_t queue_file_upload(const char *filepath, const char *url)
{
    UploadRequest request;
    strncpy(request.filepath, filepath, MAX_FILE_PATH - 1);
    strncpy(request.url, url, MAX_URL_LENGTH - 1);

    if (xQueueSend(upload_queue, &request, 0) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to queue upload request");
        return ESP_FAIL;
    }
    return ESP_OK;
}