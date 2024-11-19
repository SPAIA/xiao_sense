#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "sdcard_interface.h"
#include "wifi_interface.h"
#include "esp_crt_bundle.h"

#define MAX_FILE_PATH 64
#define MAX_URL_LENGTH 256
#define QUEUE_SIZE 10

#define MAX_FILE_SIZE (1024 * 1024) // 1MB max file size, adjust as needed

#include "esp_crt_bundle.h"
typedef struct
{
    char filepath[256];
    char url[256];
} UploadRequest;

static const char *TAG = "file_upload";
static QueueHandle_t upload_queue;

esp_err_t upload_file_to_https(const char *filepath, const char *url, const char *api_key)
{
    FILE *file = fopen(filepath, "rb");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return ESP_FAIL;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size > MAX_FILE_SIZE)
    {
        ESP_LOGE(TAG, "File too large");
        fclose(file);
        return ESP_FAIL;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach, // Use ESP-IDF's CA certificate bundle
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        fclose(file);
        return ESP_FAIL;
    }

    // Extract just the filename from the filepath
    const char *filename = strrchr(filepath, '/');
    filename = (filename != NULL) ? filename + 1 : filepath;

    // Set headers
    esp_http_client_set_header(client, "Content-Type", "multipart/form-data; boundary=------------------------boundary");
    esp_http_client_set_header(client, "Authorization", api_key);

    // Prepare multipart form data
    const char *boundary = "------------------------boundary";
    size_t buffer_size = file_size + 1024; // Extra space for headers and boundary
    char *buffer = malloc(buffer_size);
    if (buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory");
        fclose(file);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int header_size = snprintf(buffer, buffer_size,
                               "--%s\r\n"
                               "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                               "Content-Type: application/octet-stream\r\n\r\n",
                               boundary, filename);

    if (header_size < 0 || header_size >= buffer_size)
    {
        ESP_LOGE(TAG, "Multipart header size is too large");
        free(buffer);
        fclose(file);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t bytes_read = fread(buffer + header_size, 1, file_size, file);
    if (bytes_read != file_size)
    {
        ESP_LOGE(TAG, "Failed to read file completely");
        free(buffer);
        fclose(file);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int content_length = header_size + file_size;
    content_length += snprintf(buffer + content_length, buffer_size - content_length, "\r\n--%s--\r\n", boundary);

    // Set the content length and post field
    esp_http_client_set_post_field(client, buffer, content_length);

    // Perform the HTTP POST request
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP POST Status = %d", status_code);

        // If upload was successful, delete the file
        if (status_code >= 200 && status_code < 300) // Check for 2xx successful status
        {
            if (remove(filepath) == 0)
            {
                ESP_LOGI(TAG, "File successfully uploaded and deleted: %s", filepath);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to delete file: %s", filepath);
            }
        }
    }
    else
    {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    free(buffer);
    fclose(file);
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
                esp_err_t result = upload_file_to_https(request.filepath, request.url, CONFIG_SPAIA_DEVICE_ID);
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

void init_upload_queue()
{
    upload_queue = xQueueCreate(QUEUE_SIZE, sizeof(UploadRequest));
    if (upload_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create upload queue");
    }
}

void init_file_upload_system()
{
    init_upload_queue();
    xTaskCreatePinnedToCore(file_upload_task, "file_upload_task", 8192, NULL, 5, NULL, PRO_CPU_NUM);
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