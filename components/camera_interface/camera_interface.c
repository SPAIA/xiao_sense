#include <stdio.h>
#include "freertos/FreeRTOS.h"

#include "sdcard_interface.h"

#include "esp_log.h"
#include "camera_config.h"
#include "camera_interface.h"
#include "img_converters.h"

const char cameraTag[7] = "camera";

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
static camera_config_t camera_config_hires = {
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

    .xclk_freq_hz = 10000000,           // The clock frequency of the image sensor
    .fb_location = CAMERA_FB_IN_PSRAM,  // Set the frame buffer storage location
    .pixel_format = PIXFORMAT_JPEG,     // The pixel format of the image: PIXFORMAT_ + YUV422|GRAYSCALE|RGB565|JPEG
    .frame_size = FRAMESIZE_XGA,        // The resolution size of the image: FRAMESIZE_ + QVGA|CIF|VGA|SVGA|XGA|SXGA|UXGA
    .jpeg_quality = 12,                 // The quality of the JPEG image, ranging from 0 to 63.
    .fb_count = 2,                      // The number of frame buffers to use.
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY //  The image capture mode.
};

void initialize_camera(void)
{
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

    // Initial camera settings for motion detection
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL)
    {
        ESP_LOGE(cameraTag, "Failed to acquire sensor");
        return;
    }

    // Other settings remain the same
    s->set_quality(s, 10);
    s->set_brightness(s, 0);
    s->set_contrast(s, 1);
    s->set_saturation(s, 1);
    s->set_sharpness(s, 2);
    s->set_denoise(s, 1);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_ae_level(s, 0);
    s->set_aec_value(s, 300);
    s->set_gain_ctrl(s, 1);
    s->set_agc_gain(s, 0);
    s->set_gainceiling(s, GAINCEILING_2X);
    s->set_bpc(s, 1);
    s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_lenc(s, 1);
    s->set_hmirror(s, 0);
    s->set_vflip(s, 0);
    s->set_dcw(s, 1);
}

void takeHighResPhoto()
{
    ESP_LOGI(cameraTag, "Taking high-resolution picture...");

    // Reconfigure and reinitialize the camera for high-res capture
    esp_camera_deinit();                  // Deinit the current camera config
    vTaskDelay(100 / portTICK_PERIOD_MS); // Short delay

    if (esp_camera_init(&camera_config_hires) != ESP_OK) // Reinitialize with high-res config
    {
        ESP_LOGE(cameraTag, "Failed to reinitialize camera for high-res photo");
        return;
    }

    camera_fb_t *pic = esp_camera_fb_get();

    if (!pic)
    {
        ESP_LOGE(cameraTag, "Camera capture failed");
        return;
    }

    ESP_LOGI(cameraTag, "High-res picture taken! Its size was: %zu bytes", pic->len);

    saveJpegToSdcard(pic); // Save the high-res photo

    esp_camera_fb_return(pic);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    esp_camera_deinit();                  // Deinit the current camera config
    vTaskDelay(100 / portTICK_PERIOD_MS); // Short delay

    if (esp_camera_init(&camera_config) != ESP_OK) // Reinitialize with high-res config
    {
        ESP_LOGE(cameraTag, "Failed to reinitialize camera for high-res photo");
        return;
    }

    vTaskDelay(100 / portTICK_PERIOD_MS); // Short delay for settings to take effect
}

bool detect_motion(camera_fb_t *prev_frame, camera_fb_t *current_frame, float threshold)
{
    if (!prev_frame || !current_frame || prev_frame->len != current_frame->len)
    {
        ESP_LOGI(cameraTag, "frame error");
        return false;
    }

    int changed_pixels = 0;
    for (size_t i = 0; i < prev_frame->len; i++)
    {
        if (abs(prev_frame->buf[i] - current_frame->buf[i]) > 25) // Adjust sensitivity as needed
        {
            changed_pixels++;
        }
    }

    float change_percent = (float)changed_pixels / (prev_frame->width * prev_frame->height);
    return change_percent > threshold;
}

void motion_detection_task(void *pvParameters)
{
    camera_fb_t *prev_frame = NULL;
    camera_fb_t *current_frame = NULL;
    float motion_threshold = 0.1; // Adjust this value to change motion sensitivity

    while (1)
    {
        current_frame = esp_camera_fb_get();
        if (!current_frame)
        {
            ESP_LOGE(cameraTag, "Camera capture failed");
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }

        if (prev_frame)
        {

            if (detect_motion(prev_frame, current_frame, motion_threshold))
            {
                ESP_LOGI(cameraTag, "Motion detected! Taking high-res photo...");
                takeHighResPhoto();
            }

            esp_camera_fb_return(prev_frame);
        }

        prev_frame = current_frame;
        vTaskDelay(100 / portTICK_PERIOD_MS); // Adjust delay as needed
    }
}

void createCameraTask()
{
    TaskHandle_t motion_task;
    xTaskCreatePinnedToCore(
        motion_detection_task,
        "motion_detection_task",
        configMINIMAL_STACK_SIZE * 4,
        NULL,
        tskIDLE_PRIORITY + 1,
        &motion_task,
        0 // Pin to Core 0 (main core)
    );
}