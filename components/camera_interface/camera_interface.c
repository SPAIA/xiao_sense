#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sdcard_interface.h"
#include "motion_detector.h"

#include "esp_log.h"
#include "camera_config.h"
#include "camera_interface.h"
#include "img_converters.h"
#include "upload_manager.h"

const char cameraTag[7] = "camera";

SemaphoreHandle_t camera_semaphore;

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

static camera_config_t get_default_camera_config(void)
{
    camera_config_t config = {
        .pin_pwdn = PWDN_GPIO_NUM,
        .pin_reset = RESET_GPIO_NUM,
        .pin_xclk = XCLK_GPIO_NUM,
        .pin_sccb_sda = SIOD_GPIO_NUM,
        .pin_sccb_scl = SIOC_GPIO_NUM,
        .pin_d7 = Y9_GPIO_NUM,
        .pin_d6 = Y8_GPIO_NUM,
        .pin_d5 = Y7_GPIO_NUM,
        .pin_d4 = Y6_GPIO_NUM,
        .pin_d3 = Y5_GPIO_NUM,
        .pin_d2 = Y4_GPIO_NUM,
        .pin_d1 = Y3_GPIO_NUM,
        .pin_d0 = Y2_GPIO_NUM,
        .pin_vsync = VSYNC_GPIO_NUM,
        .pin_href = HREF_GPIO_NUM,
        .pin_pclk = PCLK_GPIO_NUM,

        // Memory-efficient settings to prevent fragmentation
        .xclk_freq_hz = 10000000, // Will be updated based on sensor
        .fb_location = CAMERA_FB_IN_PSRAM,
        .pixel_format = PIXFORMAT_GRAYSCALE,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 10,
        .fb_count = 1,                   // Reduced from 2 to 1 to save memory
        .grab_mode = CAMERA_GRAB_LATEST, // Changed from WHEN_EMPTY to reduce overflow
        .sccb_i2c_port = 1               // Explicitly set I2C port
    };
    return config;
}

void silence_camera_logs()
{
    esp_log_level_set("s3 ll_cam", ESP_LOG_INFO);
    esp_log_level_set("cam_hal", ESP_LOG_INFO);
    esp_log_level_set("sccb", ESP_LOG_INFO);
    esp_log_level_set("camera", ESP_LOG_INFO);
}
static void configure_sensor_settings(sensor_t *s)
{
    if (!s)
        return;

    // Basic settings
    s->set_brightness(s, 2); // Increased from 1 to 2
    s->set_contrast(s, 1);   // Increased from 0 to 1
    s->set_saturation(s, 2); // Increased from 1 to 2
    s->set_sharpness(s, 1);
    s->set_denoise(s, 1);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);     // Enable advanced exposure control
    s->set_ae_level(s, 2); // Increased from 0 to 1
    s->set_gain_ctrl(s, 1);

    // Sensor-specific optimizations
    if (s->id.PID == OV2640_PID)
    {
        s->set_gainceiling(s, GAINCEILING_4X); // Increased from 2X to 4X
        s->set_aec_value(s, 500);              // Increased from 200 to 500 for brighter images
        ESP_LOGI(cameraTag, "set ov2640");
        // Additional OV2640-specific settings for low light
        s->set_agc_gain(s, 0); // Auto gain control
        s->set_bpc(s, 0);      // Disable black pixel correction to preserve light
        s->set_wpc(s, 0);      // Disable white pixel correction
        s->set_raw_gma(s, 1);  // Enable gamma correction
        s->set_lenc(s, 1);     // Enable lens correction
        s->set_hmirror(s, 0);  // Disable horizontal mirror
        s->set_vflip(s, 0);    // Disable vertical flip
        s->set_dcw(s, 1);      // Enable downsize crop
    }
    else if (s->id.PID == OV5640_PID)
    {
        // Keep existing OV5640 settings
        s->set_gainceiling(s, GAINCEILING_2X);
        s->set_aec_value(s, 400);
    }
}

// Helper function to force memory cleanup
static void force_memory_cleanup(void)
{
    // Print memory info for debugging
    ESP_LOGI(cameraTag, "Forcing memory cleanup");

    // Force a garbage collection cycle
    vTaskDelay(pdMS_TO_TICKS(100));

    // Allocate and free a large block to help consolidate memory
    void *temp = heap_caps_malloc(32768, MALLOC_CAP_SPIRAM);
    if (temp)
    {
        memset(temp, 0, 32768);
        heap_caps_free(temp);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
}

esp_err_t initialize_camera(void)
{
    camera_semaphore = xSemaphoreCreateMutex();
    if (!camera_semaphore)
    {
        ESP_LOGE(cameraTag, "Failed to create camera semaphore");
        return ESP_FAIL;
    }

    // Force memory cleanup before initialization
    force_memory_cleanup();

    // Get initial camera config
    camera_config_t camera_config = get_default_camera_config();

    // Try with smaller frame size first
    camera_config.frame_size = FRAMESIZE_QVGA;
    camera_config.fb_count = 1;

    // First initialization attempt
    esp_err_t ret = esp_camera_init(&camera_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(cameraTag, "Camera init failed with error 0x%x, trying with minimal settings", ret);

        // Try with even smaller frame size
        camera_config.frame_size = FRAMESIZE_QQVGA;
        camera_config.pixel_format = PIXFORMAT_GRAYSCALE;
        camera_config.fb_count = 1;

        // Force memory cleanup again
        force_memory_cleanup();

        ret = esp_camera_init(&camera_config);
        if (ret != ESP_OK)
        {
            ESP_LOGE(cameraTag, "Camera init still failed with minimal settings: 0x%x", ret);
            return ret;
        }
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
            force_memory_cleanup();
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
esp_err_t switch_camera_mode(camera_config_t *config)
{
    esp_err_t err = esp_camera_deinit();
    if (err != ESP_OK)
    {
        ESP_LOGE(cameraTag, "Camera deinit failed: %s", esp_err_to_name(err));
        return err;
    }

    // Force memory cleanup
    force_memory_cleanup();

    // Try to initialize with the provided config
    err = esp_camera_init(config);
    if (err != ESP_OK)
    {
        ESP_LOGW(cameraTag, "Camera init failed with error 0x%x, trying with reduced buffer size", err);

        // Reduce buffer count and try again
        config->fb_count = 1;

        // Force memory cleanup again
        force_memory_cleanup();

        // Try again with reduced settings
        err = esp_camera_init(config);
        if (err != ESP_OK)
        {
            ESP_LOGW(cameraTag, "Camera init still failed, trying with smaller frame size");

            // Try with a smaller frame size
            framesize_t original_size = config->frame_size;
            if (config->frame_size > FRAMESIZE_QVGA)
            {
                config->frame_size = FRAMESIZE_QVGA;
            }
            else if (config->frame_size > FRAMESIZE_QQVGA)
            {
                config->frame_size = FRAMESIZE_QQVGA;
            }

            if (config->frame_size != original_size)
            {
                ESP_LOGI(cameraTag, "Reduced frame size from %d to %d", original_size, config->frame_size);

                // Force memory cleanup once more
                force_memory_cleanup();

                err = esp_camera_init(config);
                if (err != ESP_OK)
                {
                    ESP_LOGE(cameraTag, "Camera init failed with all fallback options: %s", esp_err_to_name(err));
                    return err;
                }
            }
            else
            {
                ESP_LOGE(cameraTag, "Camera init failed with smallest possible frame size: %s", esp_err_to_name(err));
                return err;
            }
        }
    }

    configure_sensor_settings(esp_camera_sensor_get());
    return ESP_OK;
}

// Try to take a photo with progressively lower resolutions until successful
static camera_fb_t *try_capture_with_fallback(camera_config_t *config, int max_attempts)
{
    framesize_t resolutions[] = {
        FRAMESIZE_SXGA, // 1280x1024
        FRAMESIZE_XGA,  // 1024x768
        FRAMESIZE_SVGA, // 800x600
        FRAMESIZE_VGA,  // 640x480
        FRAMESIZE_QVGA, // 320x240
        FRAMESIZE_QQVGA // 160x120
    };

    int start_idx = 0;
    // Find starting index based on current config
    for (int i = 0; i < sizeof(resolutions) / sizeof(resolutions[0]); i++)
    {
        if (config->frame_size == resolutions[i])
        {
            start_idx = i;
            break;
        }
    }

    camera_fb_t *pic = NULL;
    int attempts = 0;

    // Try with progressively lower resolutions
    for (int i = start_idx; i < sizeof(resolutions) / sizeof(resolutions[0]) && attempts < max_attempts; i++, attempts++)
    {
        config->frame_size = resolutions[i];
        ESP_LOGI(cameraTag, "Trying to capture with resolution %d", config->frame_size);

        if (switch_camera_mode(config) != ESP_OK)
        {
            ESP_LOGW(cameraTag, "Failed to switch camera mode to resolution %d", config->frame_size);
            continue;
        }

        pic = esp_camera_fb_get();
        if (pic)
        {
            ESP_LOGI(cameraTag, "Successfully captured image at resolution %d", config->frame_size);
            return pic;
        }

        ESP_LOGW(cameraTag, "Failed to capture image at resolution %d", config->frame_size);
        // Force memory cleanup before trying next resolution
        force_memory_cleanup();
    }

    return NULL; // Failed to capture with any resolution
}

esp_err_t takeHighResPhoto(time_t timestamp)
{
    if (xSemaphoreTake(camera_semaphore, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    // Force memory cleanup before starting
    force_memory_cleanup();

    camera_config_t highres_config = get_default_camera_config();
    custom_sensor_info_t *sensor_info = get_sensor_info();

    if (sensor_info)
    {
        highres_config.xclk_freq_hz = sensor_info->xclk_freq_hz;
    }

    // Use memory-efficient settings
    highres_config.pixel_format = PIXFORMAT_JPEG;
    highres_config.fb_count = 1;
    highres_config.jpeg_quality = 15;           // Lower quality to reduce memory usage
    highres_config.frame_size = FRAMESIZE_SXGA; // Start with high resolution, will fall back if needed

    esp_err_t ret = ESP_OK;
    if (switch_camera_mode(&highres_config) != ESP_OK)
    {
        ESP_LOGW(cameraTag, "Failed to switch to high-res mode, trying with lower resolution");

        // Try with a lower resolution
        highres_config.frame_size = FRAMESIZE_XGA;
        if (switch_camera_mode(&highres_config) != ESP_OK)
        {
            xSemaphoreGive(camera_semaphore);
            return ESP_FAIL;
        }
    }

    camera_fb_t *pic = esp_camera_fb_get();
    if (!pic)
    {
        ESP_LOGE(cameraTag, "Camera capture failed");
        ret = ESP_FAIL;
        goto exit_no_pic;
    }

    // First save the image to the SD card
    char filename[64];
    snprintf(filename, sizeof(filename), "%s/spaia/%lld.jpg", MOUNT_POINT, (long long)timestamp);

    // Create the file and write the JPEG data
    FILE *fp = fopen(filename, "wb");
    if (fp == NULL)
    {
        ESP_LOGE(cameraTag, "Failed to create file: %s", filename);
        ret = ESP_FAIL;
        goto exit;
    }

    size_t bytes_written = fwrite(pic->buf, 1, pic->len, fp);
    fclose(fp);

    if (bytes_written != pic->len)
    {
        ESP_LOGE(cameraTag, "Failed to write all data to file: %s", filename);
        ret = ESP_FAIL;
        goto exit;
    }

    ESP_LOGI(cameraTag, "JPEG saved as %s", filename);

    // Now notify the upload manager with the correct file path
    if (upload_manager_notify_new_file(filename) != ESP_OK)
    {
        ESP_LOGE(cameraTag, "Failed to queue file for upload");
        ret = ESP_FAIL;
    }

exit:
    if (pic)
    {
        esp_camera_fb_return(pic);
        pic = NULL;
    }

    // Force memory cleanup after photo capture and before switching back
    force_memory_cleanup();

exit_no_pic:
    // Switch back to motion detection mode with minimal memory settings
    camera_config_t motion_config = get_default_camera_config();
    if (sensor_info)
    {
        motion_config.xclk_freq_hz = sensor_info->xclk_freq_hz;
    }

    // Use very memory-efficient settings for motion detection
    motion_config.frame_size = FRAMESIZE_QVGA; // Smaller frame size for motion detection
    motion_config.fb_count = 1;
    motion_config.pixel_format = PIXFORMAT_GRAYSCALE; // Use grayscale to save memory

    // Try to switch back to motion detection mode
    esp_err_t switch_err = switch_camera_mode(&motion_config);
    if (switch_err != ESP_OK)
    {
        ESP_LOGE(cameraTag, "Failed to switch back to motion detection mode: %s", esp_err_to_name(switch_err));

        // Complete camera reset as a last resort
        esp_camera_deinit();
        vTaskDelay(pdMS_TO_TICKS(500));

        // Force memory cleanup
        force_memory_cleanup();

        // Try to reinitialize from scratch
        if (initialize_camera() != ESP_OK)
        {
            ESP_LOGE(cameraTag, "Critical error: Failed to reinitialize camera");
            // At this point, we might need to reboot the system
            // But for now, we'll just continue and hope for the best
        }
    }

    xSemaphoreGive(camera_semaphore);
    return ret;
}

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

                    // Add a delay after motion detection to allow background model to stabilize
                    vTaskDelay(pdMS_TO_TICKS(500)); // 500ms delay after motion detection

                    continue; // Skip the second fb_return and semaphore give
                }
                esp_camera_fb_return(frame);
            }
            xSemaphoreGive(camera_semaphore);
        }
        vTaskDelay(pdMS_TO_TICKS(50)); // Add small delay to prevent tight loop
    }
}

void createCameraTask()
{
    if (camera_semaphore == NULL)
    {
        ESP_LOGE(cameraTag, "Camera semaphore not initialized. Cannot create camera task.");
        return;
    }

    BaseType_t xReturned = xTaskCreatePinnedToCore(
        motion_detection_task,
        "motion_detection_task",
        8192, // Increased stack size
        NULL,
        tskIDLE_PRIORITY,
        NULL,
        APP_CPU_NUM);

    if (xReturned != pdPASS)
    {
        ESP_LOGE(cameraTag, "Failed to create motion detection task");
    }
    else
    {
        ESP_LOGI(cameraTag, "Motion detection task created successfully");
    }
}
