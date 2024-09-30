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

    .xclk_freq_hz = 20000000,          // The clock frequency of the image sensor
    .fb_location = CAMERA_FB_IN_PSRAM, // Set the frame buffer storage location
    .pixel_format = PIXFORMAT_JPEG,    // The pixel format of the image: PIXFORMAT_ + YUV422|GRAYSCALE|RGB565|JPEG
    .frame_size = FRAMESIZE_UXGA,      // The resolution size of the image: FRAMESIZE_ + QVGA|CIF|VGA|SVGA|XGA|SXGA|UXGA
    .jpeg_quality = 15,                // The quality of the JPEG image, ranging from 0 to 63.
    .fb_count = 2,                     // The number of frame buffers to use.
    .grab_mode = CAMERA_GRAB_LATEST    //  The image capture mode.
};

void initialize_camera(void)
{
    esp_err_t ret = esp_camera_init(&camera_config);
    if (ret == ESP_OK)
    {
        ESP_LOGI(cameraTag, "Camera configured successful");
    }
    else
    {
        ESP_LOGI(cameraTag, "Camera configured unsuccessful");
        return;
    }

    sensor_t *s = esp_camera_sensor_get();

    // s->set_framesize(s, FRAMESIZE_SXGA);   // Higher resolution for more detail
    s->set_quality(s, 10);                 // Lower quality (0-63) for faster processing
    s->set_brightness(s, 0);               // Normal brightness
    s->set_contrast(s, 1);                 // Slightly increased contrast
    s->set_saturation(s, 1);               // Slightly increased saturation
    s->set_sharpness(s, 2);                // Increased sharpness (if available)
    s->set_denoise(s, 1);                  // Enable denoise (if available)
    s->set_whitebal(s, 1);                 // Enable white balance
    s->set_awb_gain(s, 1);                 // Enable auto white balance gain
    s->set_wb_mode(s, 0);                  // Auto white balance mode
    s->set_exposure_ctrl(s, 1);            // Enable auto exposure
    s->set_aec2(s, 1);                     // Enable AEC DSP
    s->set_ae_level(s, 0);                 // Auto exposure level 0
    s->set_aec_value(s, 300);              // Lower exposure time for faster shutter
    s->set_gain_ctrl(s, 1);                // Enable auto gain control
    s->set_agc_gain(s, 0);                 // Lower gain to reduce noise
    s->set_gainceiling(s, GAINCEILING_2X); // Set gain ceiling to 2X
    s->set_bpc(s, 1);                      // Enable black pixel correct
    s->set_wpc(s, 1);                      // Enable white pixel correct
    s->set_raw_gma(s, 1);                  // Enable raw GMA
    s->set_lenc(s, 1);                     // Enable lens correction
    s->set_hmirror(s, 0);                  // Disable horizontal mirror
    s->set_vflip(s, 0);                    // Disable vertical flip
    s->set_dcw(s, 1);                      // 0 = disable , 1 = enable
}

void takePicture()
{
    ESP_LOGI(cameraTag, "Taking picture...");
    camera_fb_t *pic = esp_camera_fb_get();

    if (!pic)
    {
        ESP_LOGE(cameraTag, "Camera capture failed");
        return;
    }

    ESP_LOGI(cameraTag, "Picture taken! Its size was: %zu bytes", pic->len);

    saveJpegToSdcard(pic); // Assuming this function returns esp_err_t

    esp_camera_fb_return(pic);
}

void cameraTakePicture_5_sec(void *pvParameters)
{
    for (;;)
    {
        takePicture();

        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}
bool detect_motion(camera_fb_t *prev_frame, camera_fb_t *current_frame, float threshold)
{
    if (!prev_frame || !current_frame || prev_frame->len != current_frame->len)
    {
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

    // Change camera settings for motion detection
    sensor_t *s = esp_camera_sensor_get();
    s->set_framesize(s, FRAMESIZE_QVGA);      // Lower resolution for faster processing
    s->set_pixformat(s, PIXFORMAT_GRAYSCALE); // Grayscale for simpler processing

    while (1)
    {
        current_frame = esp_camera_fb_get();
        if (!current_frame)
        {
            ESP_LOGE(cameraTag, "Camera capture failed");
            continue;
        }

        if (prev_frame)
        {
            if (detect_motion(prev_frame, current_frame, motion_threshold))
            {
                ESP_LOGI(cameraTag, "Motion detected!");
                // Add your motion detection handling code here
                // For example, you could trigger an alert or start recording
            }

            esp_camera_fb_return(prev_frame);
        }

        prev_frame = current_frame;
        vTaskDelay(100 / portTICK_PERIOD_MS); // Adjust delay as needed
    }
}

void motion_detection_task(void *pvParameters)
{
    camera_fb_t *prev_frame = NULL;
    camera_fb_t *current_frame = NULL;
    float motion_threshold = 0.1; // Adjust this value to change motion sensitivity

    // Change camera settings for motion detection
    sensor_t *s = esp_camera_sensor_get();
    s->set_framesize(s, FRAMESIZE_QVGA);      // Lower resolution for faster processing
    s->set_pixformat(s, PIXFORMAT_GRAYSCALE); // Grayscale for simpler processing

    while (1)
    {
        current_frame = esp_camera_fb_get();
        if (!current_frame)
        {
            ESP_LOGE(cameraTag, "Camera capture failed");
            continue;
        }

        if (prev_frame)
        {
            if (detect_motion(prev_frame, current_frame, motion_threshold))
            {
                ESP_LOGI(cameraTag, "Motion detected!");
                // Add your motion detection handling code here
                // For example, you could trigger an alert or start recording
            }

            esp_camera_fb_return(prev_frame);
        }

        prev_frame = current_frame;
        vTaskDelay(100 / portTICK_PERIOD_MS); // Adjust delay as needed
    }
}

void createCameraTask()
{
    TaskHandle_t picture_task;
    xTaskCreate(
        cameraTakePicture_5_sec,
        "cameraTakePicture_5_sec",
        configMINIMAL_STACK_SIZE * 4,
        NULL,
        tskIDLE_PRIORITY,
        &picture_task);

    TaskHandle_t motion_task;
    xTaskCreatePinnedToCore(
        motion_detection_task,
        "motion_detection_task",
        configMINIMAL_STACK_SIZE * 4,
        NULL,
        tskIDLE_PRIORITY + 1, // Higher priority than the picture task
        &motion_task,
        0 // Pin to Core 0 (main core)
    );
}