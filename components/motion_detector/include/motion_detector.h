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
void update_background_model(camera_fb_t *frame);

/**
 * @brief Detect motion by comparing the current frame to the background model
 *
 * @param current_frame Pointer to the current camera frame
 * @param threshold The threshold for considering a change as motion (0.0 to 1.0)
 * @return true if motion is detected, false otherwise
 */
bool detect_motion(camera_fb_t *current_frame, float threshold);

/**
 * @brief Clean up and free the memory used by the background model
 */
void cleanup_background_model(void);

#endif // MOTION_DETECTOR_H