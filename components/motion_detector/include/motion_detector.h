#ifndef MOTION_DETECTOR_H
#define MOTION_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <esp_camera.h>
#include "esp_log.h"
#include "esp_system.h"

// Define the structure for the background model
typedef struct
{
    uint8_t *background;
    size_t width;
    size_t height;
    bool initialized;
} BackgroundModel;

typedef struct
{
    uint8_t *buf;       // Pointer to RGB565 buffer
    size_t width;       // Frame width (320 for QVGA)
    size_t height;      // Frame height (240 for QVGA)
    pixformat_t format; // PIXFORMAT_RGB565
    size_t fb_size;     // width * height * 2
} raw_frame_t;

// Function prototypes

/**
 * @brief Initialize the background model with given dimensions
 *
 * @param width The width of the frame
 * @param height The height of the frame
 */
void initialize_background_model(size_t width, size_t height);

/**
 * @brief Update the background model with a new frame
 *
 * @param frame Pointer to the current camera frame
 */
void update_background_model(const uint8_t *pixels, size_t width, size_t height);

/**
 * @brief Detect motion by comparing the current frame to the background model
 *
 * @param current_frame Pointer to the current camera frame
 * @param threshold The threshold for considering a change as motion (0.0 to 1.0)
 * @return true if motion is detected, false otherwise
 */
bool detect_motion(const uint8_t *pixels, size_t width, size_t height, float threshold, time_t *detection_timestamp);
/**
 * @brief Clean up and free the memory used by the background model
 */
void cleanup_background_model(void);

#endif // MOTION_DETECTOR_H