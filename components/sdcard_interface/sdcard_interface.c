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

#define MAX_FILE_PATH 256

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
    char full_path[MAX_FILE_PATH];
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

void saveJpegToSdcard(camera_fb_t *captureImage)
{

    // Find the next available filename
    char filename[32];

    sprintf(filename, "%s/spaia/%u_img.jpg", MOUNT_POINT, lastKnownFile++);

    // Create the file and write the JPEG data
    FILE *fp = fopen(filename, "wb");
    if (fp != NULL)
    {
        fwrite(captureImage->buf, 1, captureImage->len, fp);
        fclose(fp);
        ESP_LOGI(sdcardTag, "JPEG saved as %s", filename);
        vTaskDelay(pdMS_TO_TICKS(500));
        // queue_file_upload(filename, "https://device.spaia.earth/image");
    }
    else
    {
        ESP_LOGE(sdcardTag, "Failed to create file: %s", filename);
    }
}

void log_sensor_data_task(void *pvParameters)
{
    sensor_data_t sensor_data;
    for (;;)
    {
        if (xQueueReceive(sensor_data_queue, &sensor_data, portMAX_DELAY) == pdTRUE)
        {
            append_data_to_csv(sensor_data.temperature, sensor_data.humidity, sensor_data.pressure, sensor_data.bboxes);
        }
    }
}
void create_data_log_queue()
{
    ESP_LOGI(sdcardTag, "started Q");
    sensor_data_queue = xQueueCreate(10, sizeof(sensor_data_t));
    xTaskCreate(log_sensor_data_task, "file_upload_task", 8192, NULL, 5, NULL);
}

void initialize_sdcard()
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
        .max_files = 5,
        .allocation_unit_size = 32 * 1024};

    ESP_LOGI(sdcardTag, "Initializing SD card");

    // Use settings defined above to initialize SD card and mount FAT filesystem.
    // Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
    // Please check its source code and implement error recovery when developing
    // production applications.
    ESP_LOGI(sdcardTag, "Using SPI peripheral");

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = host.max_freq_khz,
    };
    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK)
    {
        ESP_LOGE(sdcardTag, "Failed to initialize bus.");
        return;
    }

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

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
        return;
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

    // Format FATFS
#ifdef FORMAT_SD_CARD
    ret = esp_vfs_fat_sdcard_format(MOUNT_POINT, card);
    if (ret != ESP_OK)
    {
        ESP_LOGE(sdcardTag, "Failed to format FATFS (%s)", esp_err_to_name(ret));
        return;
    }

    if (stat(file_foo, &st) == 0)
    {
        ESP_LOGI(sdcardTag, "file still exists");
        return;
    }
    else
    {
        ESP_LOGI(sdcardTag, "file doesnt exist, format done");
    }
#endif // CONFIG_EXAMPLE_FORMAT_SD_CARD
}

void deinitialise_sdcard()
{
    // All done, unmount partition and disable SPI peripheral
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    ESP_LOGI(sdcardTag, "Card unmounted");

    // deinitialize the bus after all devices are removed
    spi_bus_free(host.slot);
}

void append_data_to_csv(float temperature, float humidity, float pressure, const char *bboxes)
{
    ESP_LOGI(sdcardTag, "starting to save csv");
    time_t now;
    struct tm timeinfo;
    char filename[64];
    char filepath[128];

    // Get current time
    time(&now);
    localtime_r(&now, &timeinfo);

    // Generate filename based on current date
    strftime(filename, sizeof(filename), "%d-%m-%y.csv", &timeinfo);

    // Construct full filepath
    snprintf(filepath, sizeof(filepath), "%s/spaia/%s", MOUNT_POINT, filename);

    FILE *file = fopen(filepath, "r"); // Try to open the file in read mode first
    bool file_exists = (file != NULL);

    if (file != NULL)
    {
        fclose(file);
    }

    file = fopen(filepath, "a"); // Open the file in append mode

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

    // Write the data to the CSV file
    fprintf(file, "%lld,%f,%f,%f,%s\n",
            (long long)now, temperature, humidity, pressure, bboxes ? bboxes : "");

    // Close the file
    fclose(file);
    ESP_LOGI(sdcardTag, "Data appended successfully to CSV file: %s", filepath);
}