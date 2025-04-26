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
#define MOTION_THRESHOLD 50.0f
#define MOTION_LOOP_DELAY 150 // ms between motion checks
#define POST_SHOT_DELAY 500   // ms after capture
#define CAM_TASK_STACK 8192
#define CAM_TASK_PRIO tskIDLE_PRIORITY
#ifndef APP_CPU_NUM
#define APP_CPU_NUM 1
#endif

// ──────────────────────────────────────────────────────────────────────────────
// Globals
// ──────────────────────────────────────────────────────────────────────────────
static SemaphoreHandle_t cam_mux; // protects esp_camera API
static sensor_t *sensor = NULL;   // pointer refreshed after each init

static camera_config_t cfg_low;  // QVGA grayscale
static camera_config_t cfg_high; // SXGA jpeg

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
// Re‑init wrapper (deinit + init). Must be called with cam_mux held.
// ──────────────────────────────────────────────────────────────────────────────
static esp_err_t reinit_camera(const camera_config_t *cfg)
{
    esp_camera_deinit();
    vTaskDelay(pdMS_TO_TICKS(5)); // Allow power stabilization

    // Retry with exponential backoff
    for (int retry = 0; retry < 3; retry++)
    {
        log_heap_stats("Before cam init");
        esp_err_t err = esp_camera_init(cfg);
        log_heap_stats("After cam init");
        if (err == ESP_OK)
        {
            sensor = esp_camera_sensor_get();
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Camera init failed 0x%x (attempt %d)", err, retry + 1);
        esp_camera_deinit();
        vTaskDelay(pdMS_TO_TICKS(100 * (retry + 1)));
    }

    // Final attempt without delay
    esp_camera_deinit();
    log_heap_stats("before cam init");
    esp_err_t err = esp_camera_init(cfg);
    log_heap_stats("After cam init");
    if (err == ESP_OK)
    {
        sensor = esp_camera_sensor_get();
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Final camera init failure 0x%x", err);
    return ESP_FAIL;
}

// ──────────────────────────────────────────────────────────────────────────────
// Config builders (called once in init)
// ──────────────────────────────────────────────────────────────────────────────
static void build_configs(void)
{
    // base template
    camera_config_t base = {
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
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
    };

    cfg_low = base;
    cfg_high = base;

    // Low‑res motion config
    cfg_low.xclk_freq_hz = 10000000; // slower clock saves power
    cfg_low.pixel_format = PIXFORMAT_RGB565;
    cfg_low.frame_size = FRAMESIZE_SVGA; // 320×240
    cfg_low.jpeg_quality = 0;            // ignored
}

// ──────────────────────────────────────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────────────────────────────────────

esp_err_t camera_manager_init(void)
{
    cam_mux = xSemaphoreCreateMutex();
    if (!cam_mux)
        return ESP_ERR_NO_MEM;

    build_configs();
    log_heap_stats("Before cam init");
    esp_err_t err = esp_camera_init(&cfg_low);
    log_heap_stats("After cam init");
    if (err != ESP_OK)
    {
        return err;
    }

    // Initialise background model for motion detection (QVGA dims)
    initialize_background_model(320, 240);
    return ESP_OK;
}

bool camera_manager_motion_loop(float thresh, time_t *stamp)
{
    cam_profile_t prof = {0};
    prof.hit = esp_timer_get_time();

    if (xSemaphoreTake(cam_mux, 0) != pdTRUE)
        return false;
    sensor_gate(true);
    camera_fb_t *fb = esp_camera_fb_get();
    bool hit = false;
    if (fb)
    {
        hit = detect_motion(fb, thresh, stamp);
        esp_camera_fb_return(fb);
    }
    sensor_gate(false);
    xSemaphoreGive(cam_mux);
    return hit;
}

esp_err_t camera_manager_capture(time_t ts, cam_profile_t *prof)
{
    if (!prof)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Log heap before capture to diagnose memory issues
    log_heap_stats("Before capture");

    esp_err_t rc = ESP_FAIL;
    camera_fb_t *fb = NULL;
    FILE *file = NULL;
    char path[64] = {0};
    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    bool jpg_converted = false;

    /* ─── 1. Enter critical section ─────────────────────────────── */
    if (xSemaphoreTake(cam_mux, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    sensor_gate(true);

    /* ─── 2. Switch to high-res mode ────────────────────────────── */
    sensor = esp_camera_sensor_get();
    if (!sensor)
    {
        ESP_LOGE(TAG, "Failed to get sensor handle");
        rc = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    // Direct mode switch without reinit
    sensor->set_framesize(sensor, FRAMESIZE_SXGA);   // High-res
    sensor->set_quality(sensor, 10);                 // Lower is better quality (0-63)
    sensor->set_gainceiling(sensor, GAINCEILING_4X); // Adjust gain for better image

    // Small delay to let sensor registers update
    vTaskDelay(pdMS_TO_TICKS(20));
    prof->hi_ready = esp_timer_get_time();

    /* ─── 3. Grab a frame ───────────────────────────────────────── */
    // Clear frame buffer before capture
    esp_camera_fb_return(NULL); // Flush any pending frames

    // Take multiple frames to ensure buffer is fresh
    camera_fb_t *dummy = esp_camera_fb_get();
    if (dummy)
    {
        esp_camera_fb_return(dummy); // Discard first frame which may be corrupted
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    fb = esp_camera_fb_get();
    if (!fb)
    {
        ESP_LOGE(TAG, "Camera capture failed");
        rc = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    prof->fb_ok = esp_timer_get_time();

    /* ─── 4. Handle JPEG conversion if needed ────────────────────── */
    if (fb->format == PIXFORMAT_JPEG)
    {
        jpg_buf = fb->buf;
        jpg_len = fb->len;
        ESP_LOGI(TAG, "Using direct JPEG from camera: %d bytes", jpg_len);
    }
    else
    {
        ESP_LOGI(TAG, "Converting to JPEG from format %d", fb->format);
        // Try using a higher quality for better results - 95 instead of 80
        if (!frame2jpg(fb, 95 /*quality*/, &jpg_buf, &jpg_len))
        {
            ESP_LOGE(TAG, "JPEG conversion failed");
            rc = ESP_FAIL;
            goto cleanup;
        }
        jpg_converted = true;
        ESP_LOGI(TAG, "JPEG conversion successful: %d bytes", jpg_len);
    }

    /* ─── 5. Save to SD card ──────────────────────────────────── */
    snprintf(path, sizeof(path), "%s/spaia/%lld.jpg", MOUNT_POINT, (long long)ts);
    file = fopen(path, "wb");
    if (!file)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        rc = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    // Add JPEG header validation and correction if needed
    if (jpg_len < 2 || jpg_buf[0] != 0xFF || jpg_buf[1] != 0xD8)
    {
        ESP_LOGW(TAG, "Invalid JPEG data - missing SOI marker, attempting recovery");

        // Try to locate a valid JPEG header in the buffer
        bool found_header = false;
        for (size_t i = 0; i < jpg_len - 1; i++)
        {
            if (jpg_buf[i] == 0xFF && jpg_buf[i + 1] == 0xD8)
            {
                ESP_LOGI(TAG, "Found valid JPEG header at offset %d", i);
                jpg_buf += i;
                jpg_len -= i;
                found_header = true;
                break;
            }
        }

        if (!found_header)
        {
            ESP_LOGE(TAG, "Could not find valid JPEG header");
            rc = ESP_FAIL;
            goto cleanup;
        }
    }

    // Check for valid JPEG trailer
    if (jpg_len < 2 || jpg_buf[jpg_len - 2] != 0xFF || jpg_buf[jpg_len - 1] != 0xD9)
    {
        ESP_LOGW(TAG, "JPEG may be missing EOI marker");
    }

    ESP_LOGI(TAG, "Writing %d bytes to file", jpg_len);
    size_t written = fwrite(jpg_buf, 1, jpg_len, file);
    if (written != jpg_len)
    {
        ESP_LOGE(TAG, "SD write failed: wrote %d of %d bytes", written, jpg_len);
        rc = ESP_FAIL;
        goto cleanup;
    }

    // Ensure data is flushed to disk
    fflush(file);

    fclose(file);
    file = NULL;

    upload_manager_notify_new_file(path);
    prof->file_ok = esp_timer_get_time();
    rc = ESP_OK;

cleanup:
    /* ─── 6. Cleanup resources ─────────────────────────────────── */
    if (file)
    {
        fclose(file);
        if (rc != ESP_OK)
        {
            remove(path);
        }
    }

    if (jpg_converted && jpg_buf)
    {
        free(jpg_buf);
    }

    if (fb)
    {
        esp_camera_fb_return(fb);
    }

    // Restore low-res mode without reinit
    if (sensor)
    {
        sensor->set_framesize(sensor, FRAMESIZE_QVGA);
        sensor->set_quality(sensor, 30); // Can be lower quality for motion detection
    }
    sensor_gate(false);

    // Always ensure timing is captured
    if (prof->file_ok == 0)
    {
        prof->file_ok = esp_timer_get_time();
    }

    xSemaphoreGive(cam_mux);
    return rc;
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

        if (camera_manager_motion_loop(MOTION_THRESHOLD, &ts))
        {
            ESP_LOGI(TAG, "motion → capture");
            camera_manager_capture(ts, &prof);

            // Ensure final timestamp is recorded even on error
            if (prof.file_ok == 0)
                prof.file_ok = esp_timer_get_time(); // fallback

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
