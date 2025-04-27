#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"

#include "sdcard_config.h"
#include "sdcard_interface.h"
#include "file_upload.h"
#include "upload_manager.h"
#include "aht_interface.h" // Include AHT interface for temperature and humidity values

#define MAX_FILE_PATH 512
#define MAX_PATH_LEN 512

const char sdcardTag[7] = "sdcard";

QueueHandle_t sensor_data_queue = NULL;

uint16_t lastKnownFile = 0;

sdmmc_card_t *card;
// By default, SD card frequency is initialized to SDMMC_FREQ_DEFAULT (20MHz)
// For setting a specific frequency, use host.max_freq_khz (range 400kHz - 20MHz for SDSPI)
sdmmc_host_t host = SDSPI_HOST_DEFAULT();

esp_err_t sdcard_read_csv_files(const char *folder_path, char file_list[][MAX_FILE_PATH], int *file_count, int max_files)
{
    DIR *dir;
    struct dirent *ent;
    char full_path[MAX_PATH_LEN];
    int count = 0;

    dir = opendir(folder_path);
    if (dir == NULL)
    {
        ESP_LOGE("SDCARD", "Failed to open directory");
        return ESP_FAIL;
    }

    while ((ent = readdir(dir)) != NULL && count < max_files)
    {
        if (ent->d_type == DT_REG)
        { // If it's a regular file
            const char *ext = strrchr(ent->d_name, '.');
            if (ext && strcmp(ext, ".csv") == 0)
            { // If it's a .csv file
                snprintf(full_path, sizeof(full_path), "%s/%s", folder_path, ent->d_name);
                strncpy(file_list[count], full_path, MAX_FILE_PATH - 1);
                file_list[count][MAX_FILE_PATH - 1] = '\0'; // Ensure null-termination
                count++;
            }
        }
    }

    closedir(dir);
    *file_count = count;
    return ESP_OK;
}
void upload_folder()
{
    // Dynamically allocate memory for file list to reduce stack usage
    char (*file_list)[MAX_FILE_PATH] = malloc(MAX_FILES * MAX_FILE_PATH);
    if (file_list == NULL)
    {
        ESP_LOGE("MAIN", "Failed to allocate memory for file list");
        return;
    }

    int file_count = 0;
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/spaia", MOUNT_POINT);
    esp_err_t result = sdcard_read_csv_files(full_path, file_list, &file_count, MAX_FILES);

    if (result == ESP_OK)
    {
        ESP_LOGI("MAIN", "Found %d CSV files", file_count);
        // Queue a single file for upload
        for (int i = 0; i < file_count; i++)
        {
            ESP_LOGI("MAIN", "Queueing file %s for upload", file_list[i]);

            // Monitor stack usage before queuing
            // UBaseType_t stack_high_watermark = uxTaskGetStackHighWaterMark(NULL);
            // ESP_LOGI("MAIN", "Stack high watermark before queue: %d", stack_high_watermark);

            upload_manager_notify_new_file(file_list[i]);
            vTaskDelay(pdMS_TO_TICKS(100));
            // Monitor stack usage after queuing
            // stack_high_watermark = uxTaskGetStackHighWaterMark(NULL);
            // ESP_LOGI("MAIN", "Stack high watermark after queue: %d", stack_high_watermark);
        }
    }
    else
    {
        ESP_LOGE("MAIN", "Failed to read CSV files from SD card");
    }

    free(file_list); // Free allocated memory
}

esp_err_t saveJpegToSdcard(camera_fb_t *captureImage, time_t timestamp)
{
    if (captureImage == NULL)
    {
        ESP_LOGE(sdcardTag, "Invalid capture image pointer");
        return ESP_ERR_INVALID_ARG;
    }

    // Find the next available filename
    char filename[32];
    snprintf(filename, sizeof(filename), "%s/spaia/%lld.jpg", MOUNT_POINT, (long long)timestamp);

    // Create the file and write the JPEG data
    FILE *fp = fopen(filename, "wb");
    if (fp == NULL)
    {
        ESP_LOGE(sdcardTag, "Failed to create file: %s", filename);
        return ESP_FAIL;
    }

    size_t bytes_written = fwrite(captureImage->buf, 1, captureImage->len, fp);
    fclose(fp);

    if (bytes_written != captureImage->len)
    {
        ESP_LOGE(sdcardTag, "Failed to write all data to file: %s", filename);
        return ESP_FAIL;
    }

    ESP_LOGI(sdcardTag, "JPEG saved as %s", filename);

    // Use the upload manager to handle the file upload
    esp_err_t upload_result = upload_manager_notify_new_file(filename);
    if (upload_result != ESP_OK)
    {
        ESP_LOGE(sdcardTag, "Failed to queue file upload for %s", filename);
        return upload_result;
    }

    return ESP_OK;
}
void log_sensor_data_task(void *pvParameters)
{
    sensor_data_t sensor_data;
    for (;;)
    {
        if (xQueueReceive(sensor_data_queue, &sensor_data, portMAX_DELAY) == pdTRUE)
        {
            append_data_to_csv(sensor_data.timestamp,
                               sensor_data.temperature,
                               sensor_data.humidity,
                               sensor_data.pressure,
                               sensor_data.bboxes);

            // Only free if we own the memory
            if (sensor_data.bboxes != NULL && sensor_data.owns_bboxes)
            {
                free(sensor_data.bboxes);
                sensor_data.bboxes = NULL;
            }
        }
    }
}
void create_data_log_queue()
{
    ESP_LOGI(sdcardTag, "started Q");
    sensor_data_queue = xQueueCreate(10, sizeof(sensor_data_t));
    xTaskCreatePinnedToCore(
        log_sensor_data_task, "log_sensor_data_task", 8192, // Increased stack size
        NULL,
        tskIDLE_PRIORITY + 2, // Slightly higher priority
        NULL,
        PRO_CPU_NUM);
}

esp_err_t initialize_sdcard()
{
    esp_err_t ret;

    // If format_if_mount_failed is set to true, SD card will be partitioned and
    // formatted in case when mounting fails.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif // EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .max_files = 15,
        .allocation_unit_size = 32 * 1024};

    ESP_LOGI(sdcardTag, "Initializing SD card");

    // Use settings defined above to initialize SD card and mount FAT filesystem.
    ESP_LOGI(sdcardTag, "Using SPI peripheral");

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = host.max_freq_khz,
    };

    // Initialize the SPI bus
    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK)
    {
        ESP_LOGE(sdcardTag, "Failed to initialize bus.");
        return ret; // Return the error code
    }

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    // Mount the filesystem
    ESP_LOGI(sdcardTag, "Mounting filesystem");
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(sdcardTag, "Failed to mount filesystem. "
                                "If you want the card to be formatted, set the FORMAT_IF_MOUNT_FAILED in sdcard_config.h");
        }
        else
        {
            ESP_LOGE(sdcardTag, "Failed to initialize the card (%s). "
                                "Make sure SD card lines have pull-up resistors in place.",
                     esp_err_to_name(ret));
        }
        return ret; // Return the error code
    }
    ESP_LOGI(sdcardTag, "Filesystem mounted");

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);

    // Create "spaia" folder if it doesn't exist
    const char *spaia_folder = "/sd/spaia";
    struct stat st;
    if (stat(spaia_folder, &st) != 0)
    {
        // Folder doesn't exist, create it
        if (mkdir(spaia_folder, 0755) != 0)
        {
            ESP_LOGE(sdcardTag, "Failed to create 'spaia' folder");
            return ESP_FAIL; // Return an error code
        }
        else
        {
            ESP_LOGI(sdcardTag, "'spaia' folder created successfully");
        }
    }
    else
    {
        ESP_LOGI(sdcardTag, "'spaia' folder already exists");
    }

    // Format FATFS (if enabled)
#ifdef FORMAT_SD_CARD
    ret = esp_vfs_fat_sdcard_format(MOUNT_POINT, card);
    if (ret != ESP_OK)
    {
        ESP_LOGE(sdcardTag, "Failed to format FATFS (%s)", esp_err_to_name(ret));
        return ret; // Return the error code
    }

    if (stat(file_foo, &st) == 0)
    {
        ESP_LOGI(sdcardTag, "file still exists");
        return ESP_FAIL; // Return an error code
    }
    else
    {
        ESP_LOGI(sdcardTag, "file doesnt exist, format done");
    }
#endif // CONFIG_EXAMPLE_FORMAT_SD_CARD

    return ESP_OK; // Success
}

void deinitialise_sdcard()
{
    // All done, unmount partition and disable SPI peripheral
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    ESP_LOGI(sdcardTag, "Card unmounted");

    // deinitialize the bus after all devices are removed
    spi_bus_free(host.slot);
}

void append_data_to_csv(time_t timestamp, float temperature, float humidity, float pressure, const char *bboxes)
{
    ESP_LOGI(sdcardTag, "Starting to save CSV");

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
        ESP_LOGE(sdcardTag, "Failed to open file for appending: %s", filepath);
        return;
    }

    // If the file didn't exist, write the header
    if (!file_exists)
    {
        fprintf(file, "timestamp,temperature,humidity,pressure,bboxes\n");
        ESP_LOGI(sdcardTag, "Created new CSV file with header: %s", filepath);
    }

    // Get the latest temperature and humidity values from AHT sensor
    float aht_temperature = aht_get_temperature();
    float aht_humidity = aht_get_humidity();

    ESP_LOGI(sdcardTag, "Using AHT values - Temperature: %.2f°C, Humidity: %.2f%%",
             aht_temperature, aht_humidity);

    // Write the data to the CSV file, using AHT values for temperature and humidity
    fprintf(file, "%lld,%f,%f,%f,%s\n",
            (long long)timestamp, aht_temperature, aht_humidity, pressure, bboxes ? bboxes : "");

    // Close the file
    fclose(file);
    ESP_LOGI(sdcardTag, "Data appended successfully to CSV file: %s", filepath);

    // Notify the upload manager about the new/updated file
    // This will trigger an immediate upload if the interval is 0 (real-time mode)
    upload_manager_notify_new_file(filepath);
}
