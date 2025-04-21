# Camera Interface Component Guide

This document provides detailed information about the Camera Interface component, which is responsible for camera initialization, configuration, and image capture in the system.

## Overview

The Camera Interface component:
- Initializes and configures the ESP32 camera hardware
- Detects and optimizes settings for different camera sensors (OV2640, OV5640)
- Provides functions for capturing images at different resolutions
- Integrates with the Motion Detector for continuous frame acquisition
- Works with the Upload Manager to handle captured images

## Architecture

The Camera Interface serves as a bridge between the hardware camera and the application logic:

```
┌─────────────────────────────────────────────────────────────┐
│                     Camera Interface                         │
│                                                             │
│  ┌─────────────────┐      ┌─────────────────────────────┐  │
│  │ Hardware        │      │ Configuration               │  │
│  │ Abstraction     │      │ - Pin definitions           │  │
│  │ - ESP32 Camera  │      │ - Resolution settings       │  │
│  │   Driver        │      │ - Format settings           │  │
│  │ - Sensor        │      │ - Quality settings          │  │
│  │   Detection     │      │                             │  │
│  └─────────────────┘      └─────────────────────────────┘  │
│           │                            │                    │
│           │                            │                    │
│           ▼                            ▼                    │
│  ┌─────────────────┐      ┌─────────────────────────────┐  │
│  │ Image           │      │ Synchronization             │  │
│  │ Acquisition     │      │ - Semaphore for             │  │
│  │ - Frame capture │      │   thread-safe access        │  │
│  │ - Mode switching│      │ - Task coordination         │  │
│  │ - High-res      │      │                             │  │
│  │   photos        │      │                             │  │
│  └─────────────────┘      └─────────────────────────────┘  │
│           │                            ▲                    │
└───────────┼────────────────────────────┼────────────────────┘
            │                            │
            ▼                            │
┌───────────────────────┐      ┌────────────────────┐
│ Motion Detector       │      │ Upload Manager     │
│ - Frame analysis      │      │ - Image storage    │
│ - Motion detection    │      │ - Upload handling  │
└───────────────────────┘      └────────────────────┘
```

## Key Components

### Hardware Abstraction
- Interfaces with the ESP32 camera driver
- Detects camera sensor type (OV2640, OV5640)
- Configures hardware-specific settings

### Configuration Management
- Defines pin mappings for camera connection
- Sets resolution, format, and quality parameters
- Provides optimized settings for different use cases

### Image Acquisition
- Captures frames for motion detection
- Takes high-resolution photos when motion is detected
- Supports different image formats (JPEG, grayscale)

### Synchronization
- Uses semaphores for thread-safe camera access
- Coordinates between motion detection and image capture
- Prevents concurrent access issues

## Implementation Details

### Camera Initialization

The camera is initialized with a configuration that depends on the detected sensor:

```c
esp_err_t initialize_camera(void)
{
    camera_semaphore = xSemaphoreCreateMutex();
    if (!camera_semaphore)
    {
        ESP_LOGE(cameraTag, "Failed to create camera semaphore");
        return ESP_FAIL;
    }

    // Get initial camera config
    camera_config_t camera_config = get_default_camera_config();

    // First initialization attempt
    esp_err_t ret = esp_camera_init(&camera_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(cameraTag, "Camera init failed with error 0x%x", ret);
        return ret;
    }

    // Get sensor info and update configuration
    custom_sensor_info_t *sensor_info = get_sensor_info();
    if (sensor_info)
    {
        // Update XCLK frequency based on detected sensor
        camera_config.xclk_freq_hz = sensor_info->xclk_freq_hz;

        // Reinitialize with proper frequency if needed
        if (camera_config.xclk_freq_hz != 10000000)
        {
            esp_camera_deinit();
            ret = esp_camera_init(&camera_config);
            if (ret != ESP_OK)
            {
                ESP_LOGE(cameraTag, "Camera reinit failed with error 0x%x", ret);
                return ret;
            }
        }
    }

    // Configure sensor settings
    configure_sensor_settings(esp_camera_sensor_get());

    // Initialize motion detection
    initialize_background_model(320, 240);

    ESP_LOGI(cameraTag, "Camera initialized successfully");
    return ESP_OK;
}
```

### Sensor Detection and Configuration

The system detects the camera sensor type and optimizes settings accordingly:

```c
static custom_sensor_info_t *get_sensor_info()
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s)
    {
        ESP_LOGE(cameraTag, "Failed to get sensor");
        return NULL;
    }

    static custom_sensor_info_t info;
    info.pid = s->id.PID;

    if (s->id.PID == OV2640_PID)
    {
        info.xclk_freq_hz = 10000000;
        info.max_frame_size = FRAMESIZE_UXGA;
        ESP_LOGI(cameraTag, "OV2640 sensor detected");
    }
    else if (s->id.PID == OV5640_PID)
    {
        info.xclk_freq_hz = 20000000;
        info.max_frame_size = FRAMESIZE_QSXGA;
        ESP_LOGI(cameraTag, "OV5640 sensor detected");
    }
    else
    {
        ESP_LOGW(cameraTag, "Unknown sensor type: 0x%x", s->id.PID);
        return NULL;
    }

    return &info;
}
```

### High-Resolution Photo Capture

When motion is detected, the system captures a high-resolution photo:

```c
esp_err_t takeHighResPhoto(time_t timestamp)
{
    if (xSemaphoreTake(camera_semaphore, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    camera_config_t highres_config = get_default_camera_config();
    custom_sensor_info_t *sensor_info = get_sensor_info();

    if (sensor_info)
    {
        highres_config.xclk_freq_hz = sensor_info->xclk_freq_hz;
    }

    highres_config.frame_size = FRAMESIZE_SXGA;
    highres_config.pixel_format = PIXFORMAT_JPEG;
    highres_config.fb_count = 2;

    esp_err_t ret = ESP_OK;
    if (switch_camera_mode(&highres_config) != ESP_OK)
    {
        xSemaphoreGive(camera_semaphore);
        return ESP_FAIL;
    }

    camera_fb_t *pic = esp_camera_fb_get();
    if (!pic)
    {
        ESP_LOGE(cameraTag, "Camera capture failed");
        ret = ESP_FAIL;
        goto exit;
    }

    if (upload_manager_notify_new_file(pic) != ESP_OK)
    {
        ESP_LOGE(cameraTag, "Failed to save image");
        ret = ESP_FAIL;
    }

exit:
    vTaskDelay(pdMS_TO_TICKS(100));
    if (pic)
    {
        esp_camera_fb_return(pic);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    // Switch back to motion detection mode
    camera_config_t motion_config = get_default_camera_config();
    if (sensor_info)
    {
        motion_config.xclk_freq_hz = sensor_info->xclk_freq_hz;
    }
    switch_camera_mode(&motion_config);
    vTaskDelay(pdMS_TO_TICKS(100));

    xSemaphoreGive(camera_semaphore);
    return ret;
}
```

## Camera Modes and Configurations

### Motion Detection Mode
- Lower resolution (QVGA - 320x240)
- Grayscale format for efficient processing
- Higher frame rate for responsive detection

```c
// Default configuration (used for motion detection)
camera_config_t config = {
    // ... pin definitions ...
    .xclk_freq_hz = 10000000,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .pixel_format = PIXFORMAT_GRAYSCALE,
    .frame_size = FRAMESIZE_QVGA,
    .jpeg_quality = 10,
    .fb_count = 2,
    .grab_mode = CAMERA_GRAB_LATEST
};
```

### Photo Capture Mode
- Higher resolution (SXGA - 1280x1024)
- JPEG format for compressed storage
- Optimized for image quality

```c
highres_config.frame_size = FRAMESIZE_SXGA;
highres_config.pixel_format = PIXFORMAT_JPEG;
highres_config.fb_count = 2;
```

## Sensor-Specific Optimizations

The camera interface applies different optimizations based on the detected sensor:

### OV2640 Sensor
```c
if (s->id.PID == OV2640_PID)
{
    s->set_gainceiling(s, GAINCEILING_4X);
    s->set_aec_value(s, 500);
    // Additional OV2640-specific settings for low light
    s->set_agc_gain(s, 0);      // Auto gain control
    s->set_bpc(s, 0);           // Disable black pixel correction
    s->set_wpc(s, 0);           // Disable white pixel correction
    s->set_raw_gma(s, 1);       // Enable gamma correction
    s->set_lenc(s, 1);          // Enable lens correction
    s->set_hmirror(s, 0);       // Disable horizontal mirror
    s->set_vflip(s, 0);         // Disable vertical flip
    s->set_dcw(s, 1);           // Enable downsize crop
}
```

### OV5640 Sensor
```c
else if (s->id.PID == OV5640_PID)
{
    s->set_gainceiling(s, GAINCEILING_2X);
    s->set_aec_value(s, 400);
}
```

## Integration with Other Components

### Motion Detector
The camera interface provides frames to the motion detector and responds to motion events:

```c
void motion_detection_task(void *pvParameters)
{
    float motion_threshold = 50;
    time_t motion_timestamp;

    while (1)
    {
        if (xSemaphoreTake(camera_semaphore, portMAX_DELAY) == pdTRUE)
        {
            camera_fb_t *frame = esp_camera_fb_get();
            if (frame)
            {
                if (detect_motion(frame, motion_threshold, &motion_timestamp))
                {
                    ESP_LOGI(cameraTag, "Motion detected!");
                    esp_camera_fb_return(frame);
                    xSemaphoreGive(camera_semaphore);

                    if (takeHighResPhoto(motion_timestamp) != ESP_OK)
                    {
                        ESP_LOGE(cameraTag, "Failed to take high-res photo");
                    }

                    continue;
                }
                esp_camera_fb_return(frame);
            }
            xSemaphoreGive(camera_semaphore);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```

### Upload Manager
When a high-resolution photo is captured, it's passed to the upload manager:

```c
if (upload_manager_notify_new_file(pic) != ESP_OK)
{
    ESP_LOGE(cameraTag, "Failed to save image");
    ret = ESP_FAIL;
}
```

## Thread Safety and Synchronization

The camera interface uses a semaphore to ensure thread-safe access:

```c
SemaphoreHandle_t camera_semaphore;

// Create semaphore during initialization
camera_semaphore = xSemaphoreCreateMutex();

// Use semaphore for thread-safe access
if (xSemaphoreTake(camera_semaphore, portMAX_DELAY) == pdTRUE)
{
    // Safe to access camera
    // ...
    xSemaphoreGive(camera_semaphore);
}
```

## Performance Considerations

### Memory Usage
- Camera frames require significant memory
- PSRAM is used for frame buffers when available
- Different resolutions and formats have different memory requirements

### CPU Usage
- Camera operations can be CPU-intensive
- Mode switching requires time and resources
- Proper task priorities help balance system performance

### Power Consumption
- Camera is one of the most power-hungry components
- Consider power management strategies for battery-powered applications
- Balance between frame rate, resolution, and power consumption

## Troubleshooting

### Common Issues

1. **Camera Initialization Failures**
   - Check hardware connections (pins, power)
   - Verify PSRAM is enabled if using high resolutions
   - Check for conflicting pin assignments

2. **Poor Image Quality**
   - Adjust sensor settings (brightness, contrast, etc.)
   - Check lighting conditions
   - Verify camera lens is clean and properly focused

3. **Mode Switching Problems**
   - Add delays between mode switches
   - Ensure proper semaphore handling
   - Check for memory issues during high-resolution capture

4. **Memory Issues**
   - Reduce resolution or quality settings
   - Ensure frames are properly returned to the pool
   - Check for memory leaks in the camera handling code

## Extending the Camera Interface

### Possible Enhancements

1. **Additional Camera Models**
   - Add support for other camera sensors
   - Implement sensor-specific optimizations

2. **Advanced Image Processing**
   - Implement image enhancement algorithms
   - Add filters or effects for captured images

3. **Adaptive Configuration**
   - Dynamically adjust camera settings based on conditions
   - Implement day/night mode switching

4. **Multiple Camera Support**
   - Extend the interface to handle multiple cameras
   - Implement camera selection and switching logic
