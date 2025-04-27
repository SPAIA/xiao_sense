// camera_manager.c – dual‑mode camera driver with safe re‑inits
// ──────────────────────────────────────────────────────────────────────────────
// ‣ Low‑res QVGA GRAYSCALE for motion detection
// ‣ High‑res SXGA JPEG for captures
// We *always* re‑init the camera when switching resolution to avoid DMA size
// mismatches (cam_hal FB‑SIZE errors).
// ESP32‑S3 + OV2640/OV5640 family.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_camera.h"

#include "camera_config.h" // board pin map
#include "motion_detector.h"
#include "sdcard_interface.h"
#include "upload_manager.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char TAG[] = "cam_mgr";

// ──────────────────────────────────────────────────────────────────────────────
// Tunables
// ──────────────────────────────────────────────────────────────────────────────
#define MOTION_THRESHOLD 40.0f
#define MOTION_LOOP_DELAY 10 // ms between motion checks
#define POST_SHOT_DELAY 5000 // ms after capture
#define CAM_TASK_STACK 8192
#define CAM_TASK_PRIO tskIDLE_PRIORITY
#ifndef APP_CPU_NUM
#define APP_CPU_NUM 1
#endif

// ──────────────────────────────────────────────────────────────────────────────
// Globals
// ──────────────────────────────────────────────────────────────────────────────
static SemaphoreHandle_t cam_mux; // protects esp_camera API

static camera_config_t cfg = {
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
    .ledc_channel = LEDC_CHANNEL_0,
    .ledc_timer = LEDC_TIMER_0,
    .fb_count = 2,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .xclk_freq_hz = 10000000, // slower clock saves power
    .pixel_format = PIXFORMAT_YUV422,
    .frame_size = FRAMESIZE_SVGA,
    .jpeg_quality = 10 // ignored
};

// ──────────────────────────────────────────────────────────────────────────────
// ──────────────────────────────────────────────────────────────────────────────
// Power‑gate helper – boards like XIAO Sense wire PWDN to nothing (‐1). Guard it.
// ──────────────────────────────────────────────────────────────────────────────
#if (PWDN_GPIO_NUM >= 0)
static inline void sensor_gate(bool on)
{
    gpio_set_level(PWDN_GPIO_NUM, on ? 0 : 1);
}
#else
static inline void sensor_gate(bool on) { (void)on; }
#endif

typedef struct
{
    int64_t hit;      // A
    int64_t hi_ready; // B
    int64_t fb_ok;    // C
    int64_t file_ok;  // D
} cam_profile_t;

// debug function to save the raw grayscale array as an image
void save_grayscale_image(const uint8_t *gray_pixels, size_t width, size_t height, const char *filename)
{
    if (!gray_pixels || width == 0 || height == 0 || !filename)
    {
        ESP_LOGE(TAG, "Invalid arguments to save_grayscale_image");
        return;
    }

    FILE *file = fopen(filename, "w");
    if (!file)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", filename);
        return;
    }

    // Write PGM header
    fprintf(file, "P2\n%zu %zu\n255\n", width, height);

    // Write pixel values
    for (size_t y = 0; y < height; y++)
    {
        for (size_t x = 0; x < width; x++)
        {
            fprintf(file, "%u ", gray_pixels[y * width + x]);
        }
        fprintf(file, "\n");
    }

    fclose(file);
    ESP_LOGI(TAG, "Grayscale image saved: %s", filename);
    upload_manager_notify_new_file(filename);
}

static inline void log_profile(const cam_profile_t *p)
{
    ESP_LOGI(TAG,
             "LAG  total=%llu ms (setup=%llu + capture=%llu + sd=%llu)",
             (p->file_ok - p->hit) / 1000,
             (p->hi_ready - p->hit) / 1000,
             (p->fb_ok - p->hi_ready) / 1000,
             (p->file_ok - p->fb_ok) / 1000);
}

void log_heap_stats(const char *label)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    ESP_LOGI("HEAP", "[%s] PSRAM Free: %d bytes, Largest block: %d bytes, Blocks: %d",
             label, info.total_free_bytes, info.largest_free_block, info.free_blocks);
}

// ──────────────────────────────────────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────────────────────────────────────
// Function prototypes
esp_err_t camera_manager_capture(camera_fb_t *fb, time_t ts);

bool camera_manager_motion_loop(float thresh, time_t *stamp, camera_fb_t **fb_out)
{
    if (!fb_out)
    {
        ESP_LOGE(TAG, "Invalid fb_out parameter");
        return false;
    }

    *fb_out = NULL; // Initialize to NULL

    if (xSemaphoreTake(cam_mux, 0) != pdTRUE)
        return false;

    sensor_gate(true);
    camera_fb_t *fb = esp_camera_fb_get();
    bool hit = false;

    if (fb)
    {
        // Allocate temporary buffer for extracted Y-plane
        uint8_t *y_plane = malloc(fb->width * fb->height); // 1 byte per pixel
        if (!y_plane)
        {
            ESP_LOGE(TAG, "Failed to allocate Y-plane buffer");
            esp_camera_fb_return(fb);
            sensor_gate(false);
            xSemaphoreGive(cam_mux);
            return false;
        }

        // Extract Y channel from YUV422 buffer
        const uint8_t *src = fb->buf;
        for (int y = 0; y < fb->height; y++)
        {
            const uint8_t *row = src + y * fb->width * 2;
            for (int x = 0; x < fb->width; x++)
            {
                y_plane[y * fb->width + x] = row[x * 2]; // pick Y0 or Y1
            }
        }

        // Optional: Downscale from SVGA (800x600) to QVGA (320x240) if you want less CPU
        uint8_t *qvga_buf = malloc(320 * 240);
        if (!qvga_buf)
        {
            ESP_LOGE(TAG, "Failed to allocate QVGA buffer");
            free(y_plane);
            esp_camera_fb_return(fb);
            sensor_gate(false);
            xSemaphoreGive(cam_mux);
            return false;
        }

        // Resize using nearest-neighbor downsampling
        const int32_t x_ratio = (fb->width << 16) / 320;
        const int32_t y_ratio = (fb->height << 16) / 240;
        for (int y = 0; y < 240; y++)
        {
            int32_t sy = (y * y_ratio) >> 16;
            for (int x = 0; x < 320; x++)
            {
                int32_t sx = (x * x_ratio) >> 16;
                qvga_buf[y * 320 + x] = y_plane[sy * fb->width + sx];
            }
        }

        free(y_plane); // Done with full-size Y-plane

        // Create raw frame wrapper

        hit = detect_motion(qvga_buf, 320, 240, thresh, stamp);
        // if (hit)
        // {
        //     char filepath[128];
        //     snprintf(filepath, sizeof(filepath), "%s/spaia/%lld.pgm", MOUNT_POINT, (long long)stamp);
        //     save_grayscale_image(qvga_buf, 320, 240, filepath);
        // }

        free(qvga_buf); // Always free

        if (hit)
        {
            // Motion detected, pass original fb to caller
            *fb_out = fb;
            // Caller is responsible for esp_camera_fb_return and xSemaphoreGive
        }
        else
        {
            // No motion, clean up
            esp_camera_fb_return(fb);
            sensor_gate(false);
            xSemaphoreGive(cam_mux);
        }
    }
    else
    {
        sensor_gate(false);
        xSemaphoreGive(cam_mux);
    }

    return hit;
}

esp_err_t camera_manager_capture(camera_fb_t *fb, time_t ts)
{
    log_heap_stats("Before capture");

    esp_err_t rc = ESP_FAIL;
    FILE *file = NULL;
    char path[64] = {0};
    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    bool jpg_converted = false;

    if (!fb)
    {
        ESP_LOGE(TAG, "Null frame buffer provided");
        return ESP_ERR_INVALID_ARG;
    }

    /* ─── JPEG Handling ────────────────────────────────────────── */
    const uint8_t *final_jpg = fb->buf;
    size_t final_len = fb->len;

    if (fb->format != PIXFORMAT_JPEG)
    {
        if (!frame2jpg(fb, 85, &jpg_buf, &jpg_len))
        {
            ESP_LOGE(TAG, "JPEG conversion failed");
            rc = ESP_FAIL;
            goto cleanup;
        }
        jpg_converted = true;
        final_jpg = jpg_buf;
        final_len = jpg_len;
    }

    /* ─── JPEG Validation (Safe) ──────────────────────────────── */
    size_t header_offset = 0;
    if (final_len < 2 || final_jpg[0] != 0xFF || final_jpg[1] != 0xD8)
    {
        for (size_t i = 0; i < final_len - 1; i++)
        {
            if (final_jpg[i] == 0xFF && final_jpg[i + 1] == 0xD8)
            {
                header_offset = i;
                break;
            }
        }
    }

    /* ─── File Writing ───────────────────────────────────────── */
    snprintf(path, sizeof(path), "%s/spaia/%lld.jpg", MOUNT_POINT, (long long)ts);
    if (!(file = fopen(path, "wb")))
    {
        ESP_LOGE(TAG, "Failed to open %s", path);
        rc = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    // Write from validated offset
    size_t write_len = final_len - header_offset;
    size_t written = fwrite(final_jpg + header_offset, 1, write_len, file);

    if (written != write_len)
    {
        ESP_LOGE(TAG, "SD write failed: %d/%d", written, write_len);
        rc = ESP_FAIL;
        goto cleanup;
    }

    fflush(file);
    fclose(file);
    file = NULL;

    upload_manager_notify_new_file(path);
    rc = ESP_OK;

cleanup:
    if (file)
    {
        fclose(file);
        if (rc != ESP_OK)
            remove(path);
    }
    if (jpg_converted)
        free(jpg_buf);

    // Note: We don't return the frame buffer here as it's managed by the caller

    return rc;
}
esp_err_t camera_manager_init(void)
{
    cam_mux = xSemaphoreCreateMutex();
    if (!cam_mux)
        return ESP_ERR_NO_MEM;
    log_heap_stats("Before cam init");
    esp_err_t err = esp_camera_init(&cfg);
    log_heap_stats("After cam init");
    if (err != ESP_OK)
    {
        return err;
    }
    // Initialise background model for motion detection (QVGA dims)
    initialize_background_model(320, 240);
    return ESP_OK;
}
// ──────────────────────────────────────────────────────────────────────────────
// Background task – replacement for old motion_detection_task/createCameraTask
// ──────────────────────────────────────────────────────────────────────────────

static void camera_worker_task(void *pv)
{
    time_t ts;
    for (;;)
    {
        cam_profile_t prof = {0};
        prof.hit = esp_timer_get_time(); // A: time of motion detection

        camera_fb_t *fb = NULL;
        if (camera_manager_motion_loop(MOTION_THRESHOLD, &ts, &fb))
        {
            ESP_LOGI(TAG, "motion → capture");

            // The frame buffer and mutex are held by camera_manager_motion_loop
            // when motion is detected
            prof.fb_ok = esp_timer_get_time(); // C: time when frame is ready

            // Process the captured frame
            esp_err_t result = camera_manager_capture(fb, ts);

            // Return the frame buffer and release the mutex
            esp_camera_fb_return(fb);
            sensor_gate(false);
            xSemaphoreGive(cam_mux);

            prof.file_ok = esp_timer_get_time(); // D: time when file is written

            log_profile(&prof); // <── see the lag breakdown
            vTaskDelay(pdMS_TO_TICKS(POST_SHOT_DELAY));
        }

        vTaskDelay(pdMS_TO_TICKS(MOTION_LOOP_DELAY));
    }
}

void createCameraTask(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(
        camera_worker_task,
        "cam_worker",
        CAM_TASK_STACK,
        NULL,
        CAM_TASK_PRIO,
        NULL,
        APP_CPU_NUM);

    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "createCameraTask failed");
    }
    else
    {
        ESP_LOGI(TAG, "createCameraTask running");
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Legacy shim
// ──────────────────────────────────────────────────────────────────────────────

esp_err_t initialize_camera(void)
{
    return camera_manager_init();
}
