# SD Card Interface Component Guide

This document provides detailed information about the SD Card Interface component, which is responsible for managing local storage operations in the system.

## Overview

The SD Card Interface component:
- Initializes and manages the SD card hardware
- Provides file system operations (read, write, delete)
- Creates and maintains directory structures
- Serves as the local storage foundation for the system
- Integrates with the Upload Manager for data persistence

## Architecture

The SD Card Interface provides a reliable local storage layer for the system:

```
┌─────────────────────────────────────────────────────────────┐
│                     SD Card Interface                        │
│                                                             │
│  ┌─────────────────┐      ┌─────────────────────────────┐  │
│  │ Hardware        │      │ File System Management      │  │
│  │ Abstraction     │      │ - Mount/unmount            │  │
│  │ - SPI/SDMMC     │      │ - Directory operations     │  │
│  │   driver        │      │ - File operations          │  │
│  │ - Card detection│      │                            │  │
│  └─────────────────┘      └─────────────────────────────┘  │
│           │                            │                    │
│           │                            │                    │
│           ▼                            ▼                    │
│  ┌─────────────────┐      ┌─────────────────────────────┐  │
│  │ Error Handling  │      │ Data Organization           │  │
│  │ - Card errors   │      │ - Directory structure       │  │
│  │ - File system   │      │ - File naming conventions   │  │
│  │   errors        │      │ - Data formats (CSV, JPEG)  │  │
│  │ - Recovery      │      │                             │  │
│  └─────────────────┘      └─────────────────────────────┘  │
│                                        │                    │
└────────────────────────────────────────┼────────────────────┘
                                         │
                                         ▼
                            ┌─────────────────────────────┐
                            │ Application Components      │
                            │ - Camera Interface          │
                            │ - Upload Manager            │
                            │ - Sensor Data Storage       │
                            └─────────────────────────────┘
```

## Key Components

### Hardware Abstraction
- Interfaces with the SD card hardware via SPI or SDMMC
- Handles card detection and initialization
- Manages hardware-level errors and recovery

### File System Management
- Mounts and unmounts the FAT file system
- Provides directory creation and navigation
- Implements file operations (open, read, write, close, delete)

### Error Handling
- Detects and manages SD card errors
- Handles file system corruption
- Implements recovery mechanisms when possible

### Data Organization
- Defines directory structure for different data types
- Implements file naming conventions (e.g., date-based)
- Manages different data formats (CSV for sensor data, JPEG for images)

## Implementation Details

### SD Card Initialization

The SD card is initialized and mounted with a FAT file system:

```c
esp_err_t initialize_sdcard(void)
{
    ESP_LOGI(TAG, "Initializing SD card");

    // Initialize SPI bus for SD card
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(HOST_ID, &bus_cfg, SPI_DMA_CHAN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure SD card
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    // Configure SPI device for SD card
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = HOST_ID;

    // Mount the file system
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                     "If you want the card to be formatted, set format_if_mount_failed = true.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return ret;
    }

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);

    // Create necessary directories
    create_directory_structure();

    ESP_LOGI(TAG, "SD card initialized successfully");
    return ESP_OK;
}
```

### Directory Structure Creation

The system creates a standardized directory structure for organizing different types of data:

```c
static esp_err_t create_directory_structure(void)
{
    // Create main data directory
    if (mkdir(MOUNT_POINT "/spaia", 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Failed to create spaia directory");
        return ESP_FAIL;
    }

    // Create subdirectories for different data types
    const char *subdirs[] = {
        "/images",
        "/logs",
        "/data",
        "/config"
    };

    for (int i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
        char path[64];
        snprintf(path, sizeof(path), MOUNT_POINT "/spaia%s", subdirs[i]);
        
        if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "Failed to create directory: %s", path);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}
```

### File Operations

The SD card interface provides functions for common file operations:

```c
// Write data to a file
esp_err_t write_file(const char *filepath, const void *data, size_t size)
{
    FILE *file = fopen(filepath, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, size, file);
    fclose(file);

    if (written != size) {
        ESP_LOGE(TAG, "Failed to write data to file: %s (wrote %d of %d bytes)",
                 filepath, written, size);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "File written successfully: %s (%d bytes)", filepath, size);
    return ESP_OK;
}

// Read data from a file
esp_err_t read_file(const char *filepath, void *data, size_t max_size, size_t *actual_size)
{
    FILE *file = fopen(filepath, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", filepath);
        return ESP_FAIL;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    size_t file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    // Check if buffer is large enough
    if (file_size > max_size) {
        ESP_LOGW(TAG, "File is larger than buffer: %s (%d > %d bytes)",
                 filepath, file_size, max_size);
        file_size = max_size;
    }

    // Read the file
    size_t read_size = fread(data, 1, file_size, file);
    fclose(file);

    if (read_size != file_size) {
        ESP_LOGE(TAG, "Failed to read data from file: %s (read %d of %d bytes)",
                 filepath, read_size, file_size);
        return ESP_FAIL;
    }

    if (actual_size != NULL) {
        *actual_size = read_size;
    }

    ESP_LOGI(TAG, "File read successfully: %s (%d bytes)", filepath, read_size);
    return ESP_OK;
}

// Append data to a file
esp_err_t append_to_file(const char *filepath, const void *data, size_t size)
{
    FILE *file = fopen(filepath, "ab");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open file for appending: %s", filepath);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, size, file);
    fclose(file);

    if (written != size) {
        ESP_LOGE(TAG, "Failed to append data to file: %s (wrote %d of %d bytes)",
                 filepath, written, size);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Data appended successfully to file: %s (%d bytes)", filepath, size);
    return ESP_OK;
}

// Delete a file
esp_err_t delete_file(const char *filepath)
{
    if (unlink(filepath) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s (errno: %d)", filepath, errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "File deleted successfully: %s", filepath);
    return ESP_OK;
}
```

### CSV File Handling

The system provides specialized functions for working with CSV data files:

```c
esp_err_t append_data_to_csv(time_t timestamp, float temperature, float humidity, float pressure, const char *bboxes)
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
    snprintf(filepath, sizeof(filepath), "%s/spaia/data/%s", MOUNT_POINT, filename);

    // Check if the file exists by attempting to open it in read mode
    FILE *file = fopen(filepath, "r");
    bool file_exists = (file != NULL);

    if (file_exists) {
        fclose(file);
    }

    // Open the file in append mode
    file = fopen(filepath, "a");

    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open file for appending: %s", filepath);
        return ESP_FAIL;
    }

    // If the file didn't exist, write the header
    if (!file_exists) {
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
    upload_manager_notify_new_file(filepath);

    return ESP_OK;
}
```

## Integration with Other Components

### Camera Interface
The camera interface saves captured images to the SD card:

```c
// Example of saving a camera frame to the SD card
esp_err_t save_image_to_sd(camera_fb_t *pic, time_t timestamp)
{
    struct tm timeinfo;
    char filename[64];
    char filepath[128];

    // Get the local time from the passed timestamp
    localtime_r(&timestamp, &timeinfo);

    // Generate filename based on the current date and time
    strftime(filename, sizeof(filename), "%Y%m%d_%H%M%S.jpg", &timeinfo);

    // Construct full filepath
    snprintf(filepath, sizeof(filepath), "%s/spaia/images/%s", MOUNT_POINT, filename);

    // Write the image data to the file
    esp_err_t ret = write_file(filepath, pic->buf, pic->len);
    if (ret == ESP_OK) {
        // Notify the upload manager about the new file
        upload_manager_notify_new_file(filepath);
    }

    return ret;
}
```

### Upload Manager
The upload manager reads files from the SD card for uploading:

```c
// Example of the upload manager accessing files on the SD card
esp_err_t upload_folder(void)
{
    DIR *dir = opendir(MOUNT_POINT "/spaia/images");
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory");
        return ESP_FAIL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip directories
        if (entry->d_type == DT_DIR) {
            continue;
        }

        // Construct full filepath
        char filepath[128];
        snprintf(filepath, sizeof(filepath), "%s/spaia/images/%s", MOUNT_POINT, entry->d_name);

        // Queue the file for upload
        queue_file_upload(filepath, "https://device.spaia.earth/upload");
    }

    closedir(dir);
    return ESP_OK;
}
```

## File System Organization

### Directory Structure
The system uses a standardized directory structure:

```
/sdcard               # Mount point
  /spaia              # Main application directory
    /images           # Camera images
    /logs             # System logs
    /data             # Sensor data (CSV files)
    /config           # Configuration files
```

### File Naming Conventions
Different file types follow specific naming conventions:

- **Images**: `YYYYMMDD_HHMMSS.jpg` (timestamp-based)
- **Sensor Data**: `DD-MM-YY.csv` (date-based, one file per day)
- **Logs**: `system_YYYYMMDD.log` (date-based)
- **Configuration**: `config.json`, `wifi.json`, etc. (function-based)

## Performance Considerations

### Storage Management
- SD cards have limited write cycles
- Implement wear leveling strategies
- Monitor free space and implement cleanup policies

### File System Performance
- FAT file systems have limitations for large numbers of files
- Consider file organization to optimize access patterns
- Use appropriate buffer sizes for file operations

### Power Considerations
- SD card operations consume significant power
- Batch write operations when possible
- Consider using power-saving modes between operations

## Troubleshooting

### Common Issues

1. **Card Initialization Failures**
   - Check hardware connections (pins, power)
   - Verify card format (FAT32 is recommended)
   - Try a different SD card to rule out hardware issues

2. **File System Errors**
   - Implement file system checks and recovery
   - Consider formatting the card if corruption is detected
   - Use journaling or transaction-based approaches for critical data

3. **Performance Issues**
   - Use higher speed class SD cards
   - Optimize buffer sizes for file operations
   - Consider using multiple smaller files instead of few large ones

4. **Storage Exhaustion**
   - Implement storage monitoring
   - Create cleanup policies for old data
   - Consider compression for log files and sensor data

## Extending the SD Card Interface

### Possible Enhancements

1. **File System Improvements**
   - Implement a more robust file system (e.g., LittleFS)
   - Add journaling for critical operations
   - Implement transaction-based file operations

2. **Data Management**
   - Add data compression for efficient storage
   - Implement data retention policies
   - Create backup and restore functionality

3. **Performance Optimizations**
   - Implement caching for frequently accessed files
   - Use double-buffering for write operations
   - Optimize file access patterns

4. **Security Features**
   - Add file encryption for sensitive data
   - Implement secure deletion
   - Add access control for multi-user scenarios
