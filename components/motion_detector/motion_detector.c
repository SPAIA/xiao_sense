#include "motion_detector.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_system.h"

static const char *detectorTag = "detector";

#define ALPHA 0.06f
#define FRAME_INIT_COUNT 10 // Number of frames to capture before starting motion detection

BackgroundModel bg_model = {NULL, 0, 0, false};

static int frame_counter = 0; // Frame counter for initialization

void initialize_background_model(size_t width, size_t height)
{
    if (bg_model.background)
    {
        free(bg_model.background);
    }
    bg_model.background = (uint8_t *)malloc(width * height);
    bg_model.width = width;
    bg_model.height = height;
    bg_model.initialized = false;
    frame_counter = 0;
}

void update_background_model(camera_fb_t *frame)
{
    if (!bg_model.background || bg_model.width != frame->width || bg_model.height != frame->height)
    {
        initialize_background_model(frame->width, frame->height);
    }

    if (frame_counter < FRAME_INIT_COUNT)
    {
        memcpy(bg_model.background, frame->buf, frame->len);
        frame_counter++; // Increment counter with each frame
        if (frame_counter >= FRAME_INIT_COUNT)
        {
            bg_model.initialized = true; // Mark the background as initialized after enough frames
            ESP_LOGI(detectorTag, "Background model initialized after %d frames", frame_counter);
        }
        return; // Skip background update and motion detection until initialized
    }
    for (size_t i = 0; i < frame->len; i++)
    {
        // Adjust ALPHA if the background update is too fast or slow
        bg_model.background[i] = (uint8_t)((1 - ALPHA) * bg_model.background[i] + ALPHA * frame->buf[i]);
    }
}

bool detect_motion(camera_fb_t *current_frame, float threshold)
{
    if (!current_frame || !bg_model.background || bg_model.width != current_frame->width || bg_model.height != current_frame->height)
    {
        ESP_LOGD(detectorTag, "Frame error or background model not initialized");
        return false;
    }
    // Update the background model
    update_background_model(current_frame);

    if (!bg_model.initialized)
    {
        ESP_LOGI(detectorTag, "Background model not yet initialized");
        return false;
    }

    int changed_pixels = 0;
    for (size_t i = 0; i < current_frame->len; i++)
    {
        // Increase the change threshold (e.g., to 40) to reduce sensitivity to noise
        if (abs(bg_model.background[i] - current_frame->buf[i]) > 40)
        {
            changed_pixels++;
        }
    }

    float change_percent = (float)changed_pixels / (current_frame->width * current_frame->height);

    // Increase the threshold for motion detection if it's too sensitive
    return change_percent > threshold;
}

void cleanup_background_model(void)
{
    if (bg_model.background)
    {
        free(bg_model.background);
        bg_model.background = NULL;
    }
    bg_model.width = 0;
    bg_model.height = 0;
    bg_model.initialized = false;
    frame_counter = 0; // Reset the frame counter when cleaning up
}