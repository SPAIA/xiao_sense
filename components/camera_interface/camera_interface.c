#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sdcard_interface.h"
#include "motion_detector.h"

#include "esp_log.h"
#include "camera_config.h"
#include "camera_interface.h"
#include "img_converters.h"

const char cameraTag[7] = "camera";

SemaphoreHandle_t camera_semaphore;

static camera_config_t camera_config = {
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

    .xclk_freq_hz = 10000000,            // The clock frequency of the image sensor
    .fb_location = CAMERA_FB_IN_PSRAM,   // Set the frame buffer storage location
    .pixel_format = PIXFORMAT_GRAYSCALE, // The pixel format of the image: PIXFORMAT_ + YUV422|GRAYSCALE|RGB565|JPEG
    .frame_size = FRAMESIZE_QVGA,        // The resolution size of the image: FRAMESIZE_ + QVGA|CIF|VGA|SVGA|XGA|SXGA|UXGA
    .jpeg_quality = 12,                  // The quality of the JPEG image, ranging from 0 to 63.
    .fb_count = 2,                       // The number of frame buffers to use.
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY  //  The image capture mode.
};
void silence_camera_logs()
{
    esp_log_level_set("s3 ll_cam", ESP_LOG_ERROR);
    esp_log_level_set("cam_hal", ESP_LOG_ERROR);
    esp_log_level_set("sccb", ESP_LOG_ERROR);
    esp_log_level_set("camera", ESP_LOG_ERROR);
}
void set_camera()
{
    // Initial camera settings for motion detection
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL)
    {
        ESP_LOGE(cameraTag, "Failed to acquire sensor");
        return;
    }

    // Set brightness level (-2 to 2). 0 is the default. Adjust based on lighting.
    s->set_brightness(s, 2); // Suggest default: 0 (adjust based on environment)

    // Set contrast level (-2 to 2). 0 is normal, higher values increase contrast.
    s->set_contrast(s, 0); // Suggest default: 0 (1 for enhanced contrast)

    // Set saturation level (-2 to 2). 0 is neutral, higher values increase saturation.
    s->set_saturation(s, 1); // Suggest default: 0 (1 for more vibrant colors)

    // Set sharpness level (0 to 3). Higher values increase sharpness but may add noise.
    s->set_sharpness(s, 0); // Suggest default: 1 (2 for more detail)

    // Set denoise level (0 or 1). 1 enables noise reduction, useful in low light.
    s->set_denoise(s, 1); // Suggest default: 1 (keep noise reduction on)

    // Enable or disable white balance (0 or 1). 1 enables auto white balance.
    s->set_whitebal(s, 1); // Suggest default: 1 (auto white balance)

    // Enable or disable auto white balance gain (0 or 1). 1 enables AWB gain.
    s->set_awb_gain(s, 1); // Suggest default: 1 (auto white balance gain)

    // Set white balance mode (0: auto, 1: sunny, 2: cloudy, etc.). 0 is auto.
    s->set_wb_mode(s, 0); // Suggest default: 0 (auto white balance mode)

    // Enable or disable exposure control (0 or 1). 1 enables auto exposure.
    s->set_exposure_ctrl(s, 1); // Suggest default: 1 (auto exposure control)

    // Use two-pass auto exposure control (0 or 1). 1 provides more precise exposure.
    s->set_aec2(s, 1); // Suggest default: 1 (use two-pass auto exposure)

    // Set auto exposure level (-2 to 2). 0 is normal exposure.
    s->set_ae_level(s, 2); // Suggest default: 0 (neutral exposure level)

    // Set target exposure value (0 to 1200). Higher values for brighter images.
    s->set_aec_value(s, 800); // Suggest default: 300 (adjust based on light conditions)

    // Enable or disable auto gain control (0 or 1). 1 enables AGC.
    s->set_gain_ctrl(s, 1); // Suggest default: 1 (auto gain control)

    // Set manual gain value if AGC is disabled (0 to 30). Ignored if AGC is on.
    s->set_agc_gain(s, 0); // Suggest default: 0 (manual gain not used with AGC on)

    // Set gain ceiling (GAINCEILING_2X to GAINCEILING_128X). Higher values boost gain in low light.
    s->set_gainceiling(s, GAINCEILING_2X); // Suggest default: GAINCEILING_4X (more gain in dim light)

    // Enable or disable black pixel correction (0 or 1). 1 removes hot/dead pixels.
    s->set_bpc(s, 1); // Suggest default: 1 (enable black pixel correction)

    // Enable or disable white pixel correction (0 or 1). 1 corrects white pixels.
    s->set_wpc(s, 1); // Suggest default: 1 (enable white pixel correction)

    // Enable or disable gamma correction (0 or 1). 1 enables gamma correction.
    s->set_raw_gma(s, 1); // Suggest default: 1 (enable gamma correction)

    // Enable or disable lens correction (0 or 1). 1 corrects lens distortion.
    s->set_lenc(s, 1); // Suggest default: 1 (enable lens correction)

    // Enable or disable horizontal mirror (0 or 1). 0 is normal, 1 flips image horizontally.
    s->set_hmirror(s, 0); // Suggest default: 0 (no horizontal flip)

    // Enable or disable vertical flip (0 or 1). 0 is normal, 1 flips image vertically.
    s->set_vflip(s, 0); // Suggest default: 0 (no vertical flip, unless needed)

    // Enable or disable downsize/crop/window (0 or 1). 1 reduces image size for lower resolutions.
    s->set_dcw(s, 1); // Suggest default: 1 (use DCW to reduce image size if needed)
}
void initialize_camera(void)
{
    silence_camera_logs();
    camera_semaphore = xSemaphoreCreateMutex();
    if (camera_semaphore == NULL)
    {
        ESP_LOGE(cameraTag, "Failed to create camera semaphore");
        return;
    }

    // ESP_LOGI(cameraTag, "Camera semaphore created successfully");
    esp_err_t ret = esp_camera_init(&camera_config);
    if (ret == ESP_OK)
    {
        ESP_LOGI(cameraTag, "Camera configured successfully");
    }
    else
    {
        ESP_LOGE(cameraTag, "Camera configuration failed");
        return;
    }
    set_camera();
    initialize_background_model(320, 240);
}
esp_err_t switch_camera_mode(camera_config_t *config, framesize_t new_frame_size, pixformat_t new_pixel_format)
{
    esp_err_t err;

    // Deinitialize the camera
    err = esp_camera_deinit();
    if (err != ESP_OK)
    {
        ESP_LOGE(cameraTag, "Camera deinit failed: %s", esp_err_to_name(err));
        return err;
    }

    // Update the configuration
    config->frame_size = new_frame_size;
    config->pixel_format = new_pixel_format;

    // Add a delay after deinitialization
    // vTaskDelay(pdMS_TO_TICKS(100));

    // Reinitialize the camera with the new configuration
    err = esp_camera_init(config);
    if (err != ESP_OK)
    {
        ESP_LOGE(cameraTag, "Camera init failed: %s", esp_err_to_name(err));
        return err;
    }
    set_camera();
    return ESP_OK;
}

void takeHighResPhoto()
{
    if (xSemaphoreTake(camera_semaphore, portMAX_DELAY) == pdTRUE)
    {
        // ESP_LOGI(cameraTag, "Taking high-resolution picture...");
        switch_camera_mode(&camera_config, FRAMESIZE_SXGA, PIXFORMAT_JPEG);
        vTaskDelay(pdMS_TO_TICKS(100));
        camera_fb_t *pic = esp_camera_fb_get();
        if (!pic)
        {
            ESP_LOGE(cameraTag, "Camera capture failed");
            goto exit;
        }

        ESP_LOGI(cameraTag, "High-res picture taken! Its size was: %zu bytes", pic->len);

        if (saveJpegToSdcard(pic) != ESP_OK)
        {
            ESP_LOGE(cameraTag, "Failed to save image to SD card");
            goto exit;
        }
    exit:

        if (pic)
        {
            esp_camera_fb_return(pic);
        }
        switch_camera_mode(&camera_config, FRAMESIZE_QVGA, PIXFORMAT_GRAYSCALE);
        initialize_background_model(320, 240);
        vTaskDelay(pdMS_TO_TICKS(1000));

        xSemaphoreGive(camera_semaphore);
    }
}

void motion_detection_task(void *pvParameters)
{
    camera_fb_t *frame = NULL;

    float motion_threshold = 50;

    while (1)
    {
        if (xSemaphoreTake(camera_semaphore, portMAX_DELAY) == pdTRUE)
        {
            camera_fb_t *frame = esp_camera_fb_get();
            if (frame)
            {

                if (detect_motion(frame, motion_threshold))
                { // 5% change threshold
                    ESP_LOGI(cameraTag, "Motion detected!");
                    esp_camera_fb_return(frame);
                    xSemaphoreGive(camera_semaphore);
                    takeHighResPhoto();
                    if (xSemaphoreTake(camera_semaphore, portMAX_DELAY) != pdTRUE)
                    {
                        ESP_LOGE(cameraTag, "Failed to retake camera semaphore after high-res photo");
                    }
                }
                esp_camera_fb_return(frame);
            }
            xSemaphoreGive(camera_semaphore);
        }
        else
        {
            ESP_LOGE(cameraTag, "Failed to take camera semaphore in motion detection task");
        }
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
        tskIDLE_PRIORITY + 2, // Slightly higher priority
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