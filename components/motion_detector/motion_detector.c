#include "motion_detector.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_system.h"
#include "sdcard_interface.h"

static const char *detectorTag = "detector";

#define ALPHA 0.06f         // lower numbers respond to quicker movements, higher numbers reduce noise sensitivity
#define FRAME_INIT_COUNT 20 // Number of frames to capture before starting motion detection

typedef struct
{
    size_t x_min, y_min;
    size_t x_max, y_max;
} BoundingBox;

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
// Calculate the intersection area of two boxes
size_t calculate_intersection(BoundingBox box1, BoundingBox box2)
{
    size_t x_min = (box1.x_min > box2.x_min) ? box1.x_min : box2.x_min;
    size_t y_min = (box1.y_min > box2.y_min) ? box1.y_min : box2.y_min;
    size_t x_max = (box1.x_max < box2.x_max) ? box1.x_max : box2.x_max;
    size_t y_max = (box1.y_max < box2.y_max) ? box1.y_max : box2.y_max;

    // If boxes do not intersect, return 0
    if (x_min >= x_max || y_min >= y_max)
    {
        return 0;
    }

    // Calculate the intersection area
    return (x_max - x_min) * (y_max - y_min);
}

// Calculate the union area of two boxes
size_t calculate_union(BoundingBox box1, BoundingBox box2)
{
    size_t area1 = (box1.x_max - box1.x_min) * (box1.y_max - box1.y_min);
    size_t area2 = (box2.x_max - box2.x_min) * (box2.y_max - box2.y_min);

    size_t intersection_area = calculate_intersection(box1, box2);
    return area1 + area2 - intersection_area;
}

// Calculate IoU
float calculate_iou(BoundingBox box1, BoundingBox box2)
{
    size_t intersection = calculate_intersection(box1, box2);
    size_t union_area = calculate_union(box1, box2);

    if (union_area == 0)
    {
        return 0.0f; // To prevent division by zero
    }

    return (float)intersection / union_area;
}
// Merge two bounding boxes into one by combining their coordinates
BoundingBox merge_boxes(BoundingBox box1, BoundingBox box2)
{
    BoundingBox merged = {
        .x_min = (box1.x_min < box2.x_min) ? box1.x_min : box2.x_min,
        .y_min = (box1.y_min < box2.y_min) ? box1.y_min : box2.y_min,
        .x_max = (box1.x_max > box2.x_max) ? box1.x_max : box2.x_max,
        .y_max = (box1.y_max > box2.y_max) ? box1.y_max : box2.y_max};
    return merged;
}
// Filter out boxes that are too large (based on area)
void filter_large_boxes(BoundingBox *boxes, size_t *box_count, size_t max_area)
{
    size_t i = 0;
    while (i < *box_count)
    {
        size_t area = (boxes[i].x_max - boxes[i].x_min) * (boxes[i].y_max - boxes[i].y_min);
        if (area > max_area)
        {
            // Shift all boxes down to remove the current box
            for (size_t j = i; j < *box_count - 1; j++)
            {
                boxes[j] = boxes[j + 1];
            }
            (*box_count)--; // Reduce the count
        }
        else
        {
            i++;
        }
    }
}
// Filter overlapping boxes based on IoU threshold and merge them
void filter_and_merge_boxes(BoundingBox *boxes, size_t *box_count, float iou_threshold, size_t max_area)
{
    // First, filter out large boxes
    filter_large_boxes(boxes, box_count, max_area);

    // Merge overlapping boxes based on IoU threshold
    for (size_t i = 0; i < *box_count; i++)
    {
        for (size_t j = i + 1; j < *box_count; j++)
        {
            if (calculate_iou(boxes[i], boxes[j]) > iou_threshold)
            {
                // Merge the overlapping boxes
                BoundingBox merged_box = merge_boxes(boxes[i], boxes[j]);
                boxes[i] = merged_box; // Replace the first box with the merged one

                // Shift all boxes down to remove the second box
                for (size_t k = j; k < *box_count - 1; k++)
                {
                    boxes[k] = boxes[k + 1];
                }
                (*box_count)--; // Reduce the count
                j--;            // Recheck the current index
            }
        }
    }
}

// Filter overlapping boxes based on IoU threshold
void filter_boxes_by_iou(BoundingBox *boxes, size_t *box_count, float iou_threshold)
{
    for (size_t i = 0; i < *box_count; i++)
    {
        for (size_t j = i + 1; j < *box_count; j++)
        {
            if (calculate_iou(boxes[i], boxes[j]) > iou_threshold)
            {
                // Merge boxes or discard the duplicate one
                // For simplicity, let's just discard the second box for now
                // You can choose to merge the boxes if necessary
                for (size_t k = j; k < *box_count - 1; k++)
                {
                    boxes[k] = boxes[k + 1];
                }
                (*box_count)--; // Reduce the count
                j--;            // Recheck the current index
            }
        }
    }
}

void draw_bounding_box(camera_fb_t *frame, BoundingBox box)
{
    // Draw a box around the object on the frame
    // For simplicity, this is a placeholder as rendering depends on your display library
    ESP_LOGI(detectorTag, "Bounding Box - x_min: %d, y_min: %d, x_max: %d, y_max: %d", box.x_min, box.y_min, box.x_max, box.y_max);
}
void filter_small_boxes(BoundingBox *boxes, size_t *box_count, size_t min_area)
{
    size_t i = 0;
    while (i < *box_count)
    {
        size_t area = (boxes[i].x_max - boxes[i].x_min) * (boxes[i].y_max - boxes[i].y_min);
        if (area < min_area)
        {
            // Remove small box
            for (size_t j = i; j < *box_count - 1; j++)
            {
                boxes[j] = boxes[j + 1];
            }
            (*box_count)--;
        }
        else
        {
            i++;
        }
    }
}
void filter_edge_touching_boxes(BoundingBox *boxes, size_t *box_count, size_t frame_width, size_t frame_height)
{
    size_t i = 0;
    while (i < *box_count)
    {
        // Check if the box touches any edge
        if (boxes[i].x_min == 0 || boxes[i].y_min == 0 ||
            boxes[i].x_max == frame_width - 1 || boxes[i].y_max == frame_height - 1)
        {
            // Remove this box by shifting all subsequent boxes
            for (size_t j = i; j < *box_count - 1; j++)
            {
                boxes[j] = boxes[j + 1];
            }
            (*box_count)--; // Decrease the count of boxes
        }
        else
        {
            i++; // Move to next box only if current box was not removed
        }
    }
}
char *boxes_to_json(BoundingBox *boxes, size_t box_count)
{
    if (box_count == 0 || boxes == NULL)
    {
        return strdup("[]"); // Return an empty JSON array if there are no boxes
    }

    size_t estimated_size = box_count * 120; // Start with a larger estimate
    char *json_string = (char *)malloc(estimated_size);
    if (json_string == NULL)
    {
        ESP_LOGE("boxes_to_json", "Failed to allocate memory for JSON string");
        return NULL;
    }

    char *current_position = json_string;
    size_t remaining_size = estimated_size;

    // Start the JSON array
    int written = snprintf(current_position, remaining_size, "[");
    current_position += written;
    remaining_size -= written;

    for (size_t i = 0; i < box_count; i++)
    {
        // Format each bounding box as a JSON object
        written = snprintf(current_position, remaining_size,
                           "{\"x_min\":%zu,\"y_min\":%zu,\"x_max\":%zu,\"y_max\":%zu}",
                           boxes[i].x_min, boxes[i].y_min, boxes[i].x_max, boxes[i].y_max);

        if (written >= remaining_size)
        {
            // We’re out of space; double size with reallocation
            size_t current_length = current_position - json_string;
            estimated_size *= 2;
            char *new_string = (char *)realloc(json_string, estimated_size);
            if (new_string == NULL)
            {
                ESP_LOGE("boxes_to_json", "Failed to reallocate memory for JSON string");
                free(json_string);
                return NULL;
            }
            json_string = new_string;
            current_position = json_string + current_length;
            remaining_size = estimated_size - current_length;

            // Retry writing
            written = snprintf(current_position, remaining_size,
                               "{\"x_min\":%zu,\"y_min\":%zu,\"x_max\":%zu,\"y_max\":%zu}",
                               boxes[i].x_min, boxes[i].y_min, boxes[i].x_max, boxes[i].y_max);
        }

        current_position += written;
        remaining_size -= written;

        // Add a comma if it’s not the last box
        if (i < box_count - 1)
        {
            written = snprintf(current_position, remaining_size, ",");
            current_position += written;
            remaining_size -= written;
        }
    }

    // Close the JSON array
    snprintf(current_position, remaining_size, "]");

    // Optional: Trim memory to actual size
    size_t final_size = current_position - json_string + 1; // +1 for null terminator
    char *final_json = (char *)realloc(json_string, final_size);
    return final_json ? final_json : json_string; // Return final allocation or original
}

bool detect_motion(camera_fb_t *current_frame, float threshold, time_t *detection_timestamp)
{
    // Declare local variables
    size_t max_boxes = 30;
    size_t box_count = 0;
    size_t max_area = 10000; // the max size of a valid box
    size_t min_area = 200;
    float iou_threshold = 0.3;
    BoundingBox *boxes = (BoundingBox *)malloc(max_boxes * sizeof(BoundingBox));

    if (!current_frame || !bg_model.background || bg_model.width != current_frame->width || bg_model.height != current_frame->height)
    {
        ESP_LOGD(detectorTag, "Frame error or background model not initialized");
        return false;
    }
    // Update the background model
    update_background_model(current_frame);

    if (!bg_model.initialized)
    {
        free(boxes); // Free the allocated memory before returning
        return false;
    }

    int changed_pixels = 0;
    size_t *pixel_x = (size_t *)malloc(current_frame->len * sizeof(size_t));
    size_t *pixel_y = (size_t *)malloc(current_frame->len * sizeof(size_t));

    size_t width = current_frame->width;
    size_t height = current_frame->height;

    // Track positions of changed pixels
    for (size_t i = 0; i < current_frame->len; i++)
    {
        if (abs(bg_model.background[i] - current_frame->buf[i]) > threshold)
        {
            changed_pixels++;
            size_t x = i % width;
            size_t y = i / width;
            pixel_x[changed_pixels - 1] = x;
            pixel_y[changed_pixels - 1] = y;
        }
    }
    // Create bounding boxes from clusters of changed pixels
    if (changed_pixels > 0 && max_boxes > 0)
    {
        size_t cluster_threshold = 20; // Minimum pixel count for a cluster

        for (size_t i = 0; i < changed_pixels; i++)
        {
            if (box_count >= max_boxes)
            {
                break;
            }

            // Simple bounding box creation by finding min/max x and y coordinates of nearby changed pixels
            BoundingBox box = {
                .x_min = pixel_x[i],
                .y_min = pixel_y[i],
                .x_max = pixel_x[i],
                .y_max = pixel_y[i]};

            for (size_t j = i + 1; j < changed_pixels; j++)
            {
                if (abs((int)pixel_x[i] - (int)pixel_x[j]) < cluster_threshold &&
                    abs((int)pixel_y[i] - (int)pixel_y[j]) < cluster_threshold)
                {
                    // Expand the bounding box
                    if (pixel_x[j] < box.x_min)
                        box.x_min = pixel_x[j];
                    if (pixel_y[j] < box.y_min)
                        box.y_min = pixel_y[j];
                    if (pixel_x[j] > box.x_max)
                        box.x_max = pixel_x[j];
                    if (pixel_y[j] > box.y_max)
                        box.y_max = pixel_y[j];
                }
            }

            boxes[box_count] = box; // Store the bounding box
            box_count++;            // Increment the box count
        }
    }

    free(pixel_x);
    free(pixel_y);

    if (box_count > 0)
    {
        // ESP_LOGI(detectorTag, "unfiltered boxes.: %zu", box_count);

        // ESP_LOGI(detectorTag, "edge touching filtered out.: %zu", box_count);
        // Filter and merge boxes based on IoU threshold and max_area
        filter_and_merge_boxes(boxes, &box_count, iou_threshold, max_area);
        filter_small_boxes(boxes, &box_count, min_area);
        // ESP_LOGI(detectorTag, "small filtered out.: %zu", box_count);
        filter_edge_touching_boxes(boxes, &box_count, width, height);

        // Check if there are still any boxes after filtering and merging
        if (box_count > 0)
        {
            if (detection_timestamp != NULL)
            {
                *detection_timestamp = time(NULL);
            }
            ESP_LOGI(detectorTag, "Remaining boxes after filtering and merging: %zu", box_count);
            char *json_string = boxes_to_json(boxes, box_count);
            if (json_string != NULL)
            {
                // Create a complete sensor_data structure
                sensor_data_t sensor_data = {
                    .timestamp = time(NULL),
                    .temperature = 0, // These will be set elsewhere
                    .humidity = 0,
                    .pressure = 0,
                    .bboxes = json_string,
                    .owns_bboxes = true // This instance owns the memory
                };
                ESP_LOGI("detector", "JSON string length: %zu", strlen(json_string));
                ESP_LOGI("detector", "JSON string: %s", json_string);
                if (xQueueSend(sensor_data_queue, &sensor_data, pdMS_TO_TICKS(10)) != pdTRUE)
                {
                    ESP_LOGE("detector", "Failed to send data to the queue");
                    // Clean up if send fails
                    free(json_string);
                }
                // Don't free json_string here - it's now owned by the queue
            }
            return true;
        }
        // else
        // {
        //     // If no boxes remain after filtering and merging, log and return false
        //     ESP_LOGI(detectorTag, "No boxes remain after filtering and merging");
        // }
    }
    free(boxes);  // Always free the memory used for the boxes
    return false; // Return false if no boxes are left
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