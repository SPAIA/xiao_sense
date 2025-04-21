# Upload Manager Component Guide

This document provides detailed information about the Upload Manager component, which is responsible for coordinating data uploads from the device to the cloud server.

## Overview

The Upload Manager is a central component that:
- Manages when and how data is uploaded from the device
- Supports both real-time and interval-based upload modes
- Provides a unified interface for other components to request uploads
- Coordinates with the File Upload component to perform the actual transfers

## Architecture

The Upload Manager follows a singleton pattern and uses FreeRTOS primitives for task management and synchronization:

```
┌─────────────────────────────────────────────────────────────┐
│                     Upload Manager                          │
│                                                             │
│  ┌─────────────────┐      ┌─────────────────────────────┐  │
│  │ Configuration   │      │ Upload Task                 │  │
│  │ - Upload        │      │ - Runs in background        │  │
│  │   interval      │      │ - Handles scheduled uploads │  │
│  │ - Real-time     │      │ - Processes upload requests │  │
│  │   mode flag     │      │                             │  │
│  └─────────────────┘      └─────────────────────────────┘  │
│           │                            ▲                    │
│           │                            │                    │
│           ▼                            │                    │
│  ┌─────────────────┐      ┌─────────────────────────────┐  │
│  │ Synchronization │      │ Event Handling              │  │
│  │ - Mutex for     │      │ - Upload trigger events     │  │
│  │   config access │      │ - Configuration change      │  │
│  │ - Event group   │      │   events                    │  │
│  │   for signaling │      │                             │  │
│  └─────────────────┘      └─────────────────────────────┘  │
│                                        │                    │
└────────────────────────────────────────┼────────────────────┘
                                         │
                                         ▼
                            ┌─────────────────────────────┐
                            │ File Upload Component       │
                            │ - Handles HTTP(S) transfers │
                            │ - Manages upload queue      │
                            └─────────────────────────────┘
```

## Key Components

### Configuration Management
- Upload interval setting (0 = real-time, >0 = interval in seconds)
- Last upload timestamp tracking
- Thread-safe access via mutex

### Upload Task
- Long-running background task
- Handles scheduled uploads based on interval
- Responds to immediate upload requests
- Manages the upload timing logic

### Event Handling
- Uses FreeRTOS event groups for signaling
- Two main event types:
  - `UPLOAD_TRIGGER_BIT`: Request for immediate upload
  - `UPLOAD_CONFIG_BIT`: Configuration has changed

### External Interface
- Simple API for other components to interact with the upload system
- Notification mechanism for new files

## Upload Modes

### Real-time Mode (interval = 0)
- Files are uploaded immediately when created
- `upload_manager_notify_new_file()` triggers an immediate upload
- Suitable for time-sensitive data or when immediate cloud processing is needed

### Interval Mode (interval > 0)
- Files are queued and uploaded at scheduled intervals
- Reduces power consumption and network usage
- Better for battery-powered deployments or limited connectivity scenarios
- Uploads occur:
  1. At regular intervals defined by the configuration
  2. When manually triggered via `upload_manager_upload_now()`

## API Reference

### Initialization

```c
esp_err_t upload_manager_init(uint32_t upload_interval_seconds);
```
- Initializes the upload manager with the specified interval
- Creates necessary synchronization primitives and background task
- Sets up the initial configuration

### Configuration

```c
esp_err_t upload_manager_set_interval(uint32_t upload_interval_seconds);
```
- Changes the upload interval at runtime
- Can switch between real-time and interval modes
- Thread-safe implementation

### Upload Control

```c
esp_err_t upload_manager_upload_now(void);
```
- Triggers an immediate upload regardless of the current interval setting
- Useful for manual uploads or critical data

### File Notification

```c
esp_err_t upload_manager_notify_new_file(const char *filename);
```
- Notifies the upload manager that a new file has been created
- In real-time mode, triggers an immediate upload
- In interval mode, the file will be uploaded at the next scheduled time

## Implementation Details

### Singleton Pattern
The upload manager is implemented as a singleton with a static instance:

```c
static upload_manager_t upload_manager = {
    .upload_interval = 0, // Default to real-time uploads
    .last_upload_time = 0,
    .config_mutex = NULL,
    .upload_task_handle = NULL,
    .event_group = NULL,
    .is_initialized = false
};
```

### Task Management
The upload task uses adaptive waiting to balance responsiveness and efficiency:

```c
// Calculate time until next upload
uint32_t remaining_seconds = current_interval - elapsed_seconds;

// Don't wait for more than 10 minutes to stay responsive
uint32_t max_wait_seconds = 600; // 10 minutes
uint32_t wait_seconds = (remaining_seconds < max_wait_seconds) ? 
                         remaining_seconds : max_wait_seconds;

wait_time = pdMS_TO_TICKS(wait_seconds * 1000);
```

### Thread Safety
All configuration access is protected by a mutex:

```c
if (xSemaphoreTake(upload_manager.config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    // Safe to access configuration here
    xSemaphoreGive(upload_manager.config_mutex);
}
```

## Current Configuration

The system is currently configured in real-time mode (interval = 0) in `main.c`:

```c
upload_manager_init(0);
```

This means that files are uploaded immediately when they are created, rather than being queued for later upload.

## Integration with Other Components

### Camera Interface
When motion is detected, the camera interface captures a high-resolution photo and notifies the upload manager:

```c
if (upload_manager_notify_new_file(pic) != ESP_OK) {
    ESP_LOGE(cameraTag, "Failed to save image");
    ret = ESP_FAIL;
}
```

### Sensor Data
Sensor data is collected and stored in CSV files, which are then notified to the upload manager:

```c
// Notify the upload manager about the new/updated file
upload_manager_notify_new_file(filepath);
```

## Changing Upload Behavior

To change from real-time to interval-based uploads:

1. Modify the initialization in `main.c`:
   ```c
   // Change from:
   upload_manager_init(0);
   
   // To (e.g., upload every 5 minutes):
   upload_manager_init(300);
   ```

2. Or dynamically change the mode at runtime:
   ```c
   // Switch to interval mode (e.g., every 10 minutes)
   upload_manager_set_interval(600);
   
   // Switch back to real-time mode
   upload_manager_set_interval(0);
   ```

## Troubleshooting

### Common Issues

1. **Uploads not occurring in real-time mode**
   - Check WiFi connectivity
   - Verify that `upload_manager_notify_new_file()` is being called
   - Ensure the file exists and is accessible

2. **Interval-based uploads not occurring**
   - Check if the interval is set correctly (non-zero value)
   - Verify that the upload task is running
   - Check system time and tick count for potential overflows

3. **Upload failures**
   - Check server connectivity
   - Verify server URL and authentication
   - Check file permissions and SD card status
