#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "sdcard_interface.h"
#include "file_upload.h"
#include "wifi_interface.h"

// Function prototype declaration
static inline uint32_t min_uint32(uint32_t a, uint32_t b);

static const char *TAG = "upload_manager";

// Function implementation
static inline uint32_t min_uint32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

// Upload manager configuration
typedef struct
{
    uint32_t upload_interval;        // Interval in seconds between uploads (0 = real-time)
    TickType_t last_upload_time;     // Last time an upload was performed (in ticks)
    SemaphoreHandle_t config_mutex;  // Mutex for configuration protection
    TaskHandle_t upload_task_handle; // Task handle for upload task
    EventGroupHandle_t event_group;  // Event group for signaling the task
    bool is_initialized;             // Initialization flag
    uint8_t failed_attempts;         // Count of consecutive failed upload attempts
    uint32_t max_backoff_ms;         // Maximum backoff time in milliseconds
    uint32_t initial_backoff_ms;     // Initial backoff time in milliseconds
} upload_manager_t;

// Event bits for the event group
#define UPLOAD_TRIGGER_BIT (1 << 0)
#define UPLOAD_CONFIG_BIT (1 << 1)

// Singleton instance
static upload_manager_t upload_manager = {
    .upload_interval = 0, // Default to real-time uploads
    .last_upload_time = 0,
    .config_mutex = NULL,
    .upload_task_handle = NULL,
    .event_group = NULL,
    .is_initialized = false,
    .failed_attempts = 0,
    .max_backoff_ms = 32000,     // 32 seconds max backoff
    .initial_backoff_ms = 1000}; // 1 second initial backoff

// Forward declarations
static void upload_task(void *pvParameters);

// Initialize the upload manager with backoff configuration
esp_err_t upload_manager_init_ex(uint32_t upload_interval_seconds, uint32_t initial_backoff_ms, uint32_t max_backoff_ms)
{
    if (upload_manager.is_initialized)
    {
        ESP_LOGW(TAG, "Upload manager already initialized");
        return ESP_OK;
    }

    // Create mutex for configuration protection
    upload_manager.config_mutex = xSemaphoreCreateMutex();
    if (!upload_manager.config_mutex)
    {
        ESP_LOGE(TAG, "Failed to create config mutex");
        return ESP_FAIL;
    }

    // Create event group for task signaling
    upload_manager.event_group = xEventGroupCreate();
    if (!upload_manager.event_group)
    {
        ESP_LOGE(TAG, "Failed to create event group");
        vSemaphoreDelete(upload_manager.config_mutex);
        upload_manager.config_mutex = NULL;
        return ESP_FAIL;
    }

    // Set initial configuration
    upload_manager.upload_interval = upload_interval_seconds;
    upload_manager.last_upload_time = xTaskGetTickCount();
    upload_manager.failed_attempts = 0;
    upload_manager.initial_backoff_ms = initial_backoff_ms;
    upload_manager.max_backoff_ms = max_backoff_ms;

    // Create upload task
    BaseType_t result = xTaskCreatePinnedToCore(
        upload_task,                        // Task function
        "upload_task",                      // Task name
        4096,                               // Stack size (in words)
        NULL,                               // Task parameters
        2,                                  // Task priority (lower priority to save power)
        &upload_manager.upload_task_handle, // Task handle
        APP_CPU_NUM                         // Core ID (usually APP_CPU for background tasks)
    );

    if (result != pdPASS || upload_manager.upload_task_handle == NULL)
    {
        ESP_LOGE(TAG, "Failed to create upload task");
        vEventGroupDelete(upload_manager.event_group);
        vSemaphoreDelete(upload_manager.config_mutex);
        upload_manager.config_mutex = NULL;
        upload_manager.event_group = NULL;
        return ESP_FAIL;
    }

    upload_manager.is_initialized = true;

    if (upload_interval_seconds == 0)
    {
        ESP_LOGI(TAG, "Upload manager initialized in real-time mode");
    }
    else
    {
        ESP_LOGI(TAG, "Upload manager initialized with %lu second interval",
                 (unsigned long)upload_interval_seconds);
    }

    return ESP_OK;
}

// Initialize the upload manager with default backoff settings
esp_err_t upload_manager_init(uint32_t upload_interval_seconds)
{
    return upload_manager_init_ex(upload_interval_seconds,
                                  upload_manager.initial_backoff_ms,
                                  upload_manager.max_backoff_ms);
}

// Set upload interval
esp_err_t upload_manager_set_interval(uint32_t upload_interval_seconds)
{
    if (!upload_manager.is_initialized)
    {
        ESP_LOGE(TAG, "Upload manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(upload_manager.config_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        upload_manager.upload_interval = upload_interval_seconds;
        // Reset the last upload time to now to avoid immediate upload after changing interval
        upload_manager.last_upload_time = xTaskGetTickCount();
        xSemaphoreGive(upload_manager.config_mutex);

        // Signal the task that configuration has changed
        xEventGroupSetBits(upload_manager.event_group, UPLOAD_CONFIG_BIT);

        if (upload_interval_seconds == 0)
        {
            ESP_LOGI(TAG, "Upload interval changed to real-time mode");
        }
        else
        {
            ESP_LOGI(TAG, "Upload interval changed to %lu seconds",
                     (unsigned long)upload_interval_seconds);
        }
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to take mutex when changing upload interval");
    return ESP_FAIL;
}

// Request an immediate upload
esp_err_t upload_manager_upload_now(void)
{
    if (!upload_manager.is_initialized)
    {
        ESP_LOGE(TAG, "Upload manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Manual upload requested");

    // For immediate upload, set the trigger bit and wake up the task
    xEventGroupSetBits(upload_manager.event_group, UPLOAD_TRIGGER_BIT);

    return ESP_OK;
}

// Notify the upload manager that a new file has been created
esp_err_t upload_manager_notify_new_file(const char *filename)
{
    if (!upload_manager.is_initialized)
    {
        ESP_LOGE(TAG, "Upload manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t current_interval;

    // Get current interval setting (protected by mutex)
    if (xSemaphoreTake(upload_manager.config_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        current_interval = upload_manager.upload_interval;
        xSemaphoreGive(upload_manager.config_mutex);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to take mutex when notifying new file");
        return ESP_FAIL;
    }

    // If real-time uploading is enabled (interval = 0), upload the file immediately
    if (current_interval == 0)
    {
        ESP_LOGI(TAG, "Real-time upload mode: uploading %s", filename);
        return queue_file_upload(filename, "https://device.spaia.earth/upload");
    }
    else
    {
        ESP_LOGI(TAG, "Interval upload mode: file %s will be uploaded at next scheduled time", filename);
        return ESP_OK;
    }
}

// Internal upload task
static void upload_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Upload task started");
    bool wifi_was_enabled = false;

    while (1)
    {
        uint32_t current_interval;
        TickType_t last_upload;
        TickType_t current_time = xTaskGetTickCount();
        TickType_t wait_time = portMAX_DELAY; // Default to indefinite wait

        // Get current configuration (protected by mutex)
        if (xSemaphoreTake(upload_manager.config_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            current_interval = upload_manager.upload_interval;
            last_upload = upload_manager.last_upload_time;
            xSemaphoreGive(upload_manager.config_mutex);
        }
        else
        {
            ESP_LOGW(TAG, "Failed to take mutex in upload task");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // If in interval mode (not real-time), calculate wait time until next upload
        if (current_interval > 0)
        {
            TickType_t elapsed_ticks = current_time - last_upload;
            uint32_t elapsed_seconds = pdTICKS_TO_MS(elapsed_ticks) / 1000;

            if (elapsed_seconds >= current_interval)
            {
                // Time to upload now
                wait_time = 0;
            }
            else
            {
                // Calculate time until next upload
                uint32_t remaining_seconds = current_interval - elapsed_seconds;

                // Don't wait for more than 10 minutes to stay responsive
                uint32_t max_wait_seconds = 600; // 10 minutes
                uint32_t wait_seconds = (remaining_seconds < max_wait_seconds) ? remaining_seconds : max_wait_seconds;

                wait_time = pdMS_TO_TICKS(wait_seconds * 1000);
                ESP_LOGD(TAG, "Upload task waiting for %lu seconds", (unsigned long)wait_seconds);
            }
        }
        else
        {
            // In real-time mode, wait indefinitely for events
            wait_time = portMAX_DELAY;
        }

        // Wait for an event or timeout
        EventBits_t bits = xEventGroupWaitBits(
            upload_manager.event_group,
            UPLOAD_TRIGGER_BIT | UPLOAD_CONFIG_BIT,
            pdTRUE,  // Clear bits on exit
            pdFALSE, // Wait for any bit
            wait_time);

        // Check if triggered by event or timeout
        bool do_upload = false;

        if ((bits & UPLOAD_TRIGGER_BIT) != 0)
        {
            ESP_LOGI(TAG, "Upload triggered by event");
            do_upload = true;
        }
        else if ((bits & UPLOAD_CONFIG_BIT) != 0)
        {
            ESP_LOGI(TAG, "Upload configuration changed");
            // Just continue the loop with the new configuration
            continue;
        }
        else if (wait_time != portMAX_DELAY)
        {
            // Timeout occurred, check if it's time to upload
            current_time = xTaskGetTickCount();

            if (xSemaphoreTake(upload_manager.config_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                current_interval = upload_manager.upload_interval;
                last_upload = upload_manager.last_upload_time;
                xSemaphoreGive(upload_manager.config_mutex);

                TickType_t elapsed_ticks = current_time - last_upload;
                uint32_t elapsed_seconds = pdTICKS_TO_MS(elapsed_ticks) / 1000;

                if (current_interval > 0 && elapsed_seconds >= current_interval)
                {
                    ESP_LOGI(TAG, "Upload triggered by interval (%lu seconds)",
                             (unsigned long)current_interval);
                    do_upload = true;
                }
            }
        }

        // Perform upload if needed
        if (do_upload)
        {
            // Check if WiFi is already connected
            wifi_was_enabled = is_wifi_connected();

            // If WiFi is not connected, enable it for the upload
            if (!wifi_was_enabled)
            {
                ESP_LOGI(TAG, "Enabling WiFi for scheduled upload...");
                esp_err_t wifi_result = wifi_enable();
                if (wifi_result != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to enable WiFi, cannot perform upload");
                    continue; // Skip this upload and wait for the next one
                }
                // Give some time for WiFi to fully connect and stabilize
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            // Check if WiFi is connected before attempting upload
            if (is_wifi_connected())
            {
                ESP_LOGI(TAG, "Performing upload");
                esp_err_t upload_result = upload_all_files();

                // Update the last upload time and reset backoff on success
                if (xSemaphoreTake(upload_manager.config_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                {
                    if (upload_result == ESP_OK)
                    {
                        upload_manager.last_upload_time = xTaskGetTickCount();
                        upload_manager.failed_attempts = 0;
                        ESP_LOGI(TAG, "Upload successful, reset backoff");
                    }
                    else
                    {
                        upload_manager.failed_attempts++;
                        uint32_t backoff_time = upload_manager.initial_backoff_ms *
                                                (1 << (upload_manager.failed_attempts - 1));
                        backoff_time = min_uint32(backoff_time, upload_manager.max_backoff_ms);
                        ESP_LOGE(TAG, "Upload failed (attempt %d), backing off for %lums",
                                 upload_manager.failed_attempts, (unsigned long)backoff_time);
                        vTaskDelay(pdMS_TO_TICKS(backoff_time));
                    }
                    xSemaphoreGive(upload_manager.config_mutex);
                }

                // If WiFi was disabled before the upload, disable it again to save power
                if (!wifi_was_enabled)
                {
                    ESP_LOGI(TAG, "Disabling WiFi after scheduled upload to save power...");
                    wifi_disable();
                }
            }
            else
            {
                ESP_LOGE(TAG, "WiFi not connected, cannot perform upload");
            }
        }
    }
}

// Modify the existing append_data_to_csv function to use the upload manager
// This is just an example of how to modify the existing function
esp_err_t modified_append_data_to_csv(time_t timestamp, float temperature, float humidity, float pressure, const char *bboxes)
{
    ESP_LOGI(TAG, "Starting to save CSV");

    struct tm timeinfo;
    char filename[64];
    char filepath[128];

    // Get the local time from the passed timestamp
    localtime_r(&timestamp, &timeinfo);

    // Generate filename based on the current date
    strftime(filename, sizeof(filename), "%d-%m-%y.csv", &timeinfo);

    // Construct full filepath
    snprintf(filepath, sizeof(filepath), "%s/spaia/%s", MOUNT_POINT, filename);

    // Check if the file exists by attempting to open it in read mode
    FILE *file = fopen(filepath, "r");
    bool file_exists = (file != NULL);

    if (file_exists)
    {
        fclose(file);
    }

    // Open the file in append mode
    file = fopen(filepath, "a");

    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for appending: %s", filepath);
        return ESP_FAIL;
    }

    // If the file didn't exist, write the header
    if (!file_exists)
    {
        fprintf(file, "timestamp,temperature,humidity,pressure,bboxes\n");
        ESP_LOGI(TAG, "Created new CSV file with header: %s", filepath);
    }

    // Write the data to the CSV file
    fprintf(file, "%lld,%f,%f,%f,%s\n",
            (long long)timestamp, temperature, humidity, pressure, bboxes ? bboxes : "");

    // Close the file
    fclose(file);
    ESP_LOGI(TAG, "Data appended successfully to CSV file: %s", filepath);

    // Notify the upload manager about the new/updated file
    // This will trigger an immediate upload if the interval is 0 (real-time mode)
    upload_manager_notify_new_file(filepath);

    return ESP_OK;
}
