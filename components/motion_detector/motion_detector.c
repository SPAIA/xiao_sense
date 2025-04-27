#include "motion_detector.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_system.h"
#include "sdcard_interface.h"

static const char *detectorTag = "detector";

// Configuration parameters
#define ALPHA 0.1f          // lower numbers respond to quicker movements, higher numbers reduce noise sensitivity
#define FRAME_INIT_COUNT 20 // Number of frames to capture before starting motion detection
#define MAX_COMPONENTS 30
#define NEIGHBORHOOD 1          // defines 8-connected neighborhood
#define MIN_COMPONENT_PIXELS 20 // Minimum pixel count for a valid component
#define MAX_BOX_AREA 10000      // Maximum area for a valid bounding box
#define MIN_BOX_AREA 30         // Minimum area for a valid bounding box
#define IOU_THRESHOLD 0.4       // Threshold for IoU-based box merging

// External queue for sensor data
extern QueueHandle_t sensor_data_queue;

// Type definitions
typedef struct
{
    int label;
    size_t x_min, y_min, x_max, y_max;
    size_t pixel_count;
} Component;

typedef struct
{
    size_t x_min, y_min;
    size_t x_max, y_max;
} BoundingBox;

// Global variables
BackgroundModel bg_model = {NULL, 0, 0, false};
static int frame_counter = 0; // Frame counter for initialization

// Initialize background model
void initialize_background_model(size_t width, size_t height)
{
    if (bg_model.background)
    {
        free(bg_model.background);
    }
    bg_model.background = (uint8_t *)malloc(width * height);
    if (!bg_model.background)
    {
        ESP_LOGE(detectorTag, "Failed to allocate memory for background model");
        return;
    }

    bg_model.width = width;
    bg_model.height = height;
    bg_model.initialized = false;
    frame_counter = 0;

    ESP_LOGI(detectorTag, "Background model initialized with dimensions %dx%d", width, height);
}

// Update background model with new frame
void update_background_model(const uint8_t *pixels, size_t width, size_t height)
{
    if (!pixels || width == 0 || height == 0)
    {
        ESP_LOGE(detectorTag, "Invalid grayscale input to update_background_model");
        return;
    }

    if (!bg_model.background || bg_model.width != width || bg_model.height != height)
    {
        initialize_background_model(width, height);
    }

    if (frame_counter < FRAME_INIT_COUNT)
    {
        if (frame_counter == 0)
        {
            memcpy(bg_model.background, pixels, width * height);
        }
        else
        {
            for (size_t i = 0; i < width * height; i++)
            {
                bg_model.background[i] = (bg_model.background[i] * frame_counter + pixels[i]) / (frame_counter + 1);
            }
        }
        frame_counter++;

        if (frame_counter >= FRAME_INIT_COUNT)
        {
            bg_model.initialized = true;
            ESP_LOGI(detectorTag, "Background model initialized after %d frames", frame_counter);
        }
        return;
    }

    // Update background model using exponential moving average
    for (size_t i = 0; i < width * height; i++)
    {
        bg_model.background[i] = (uint8_t)((1.0f - ALPHA) * bg_model.background[i] + ALPHA * pixels[i]);
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

// Calculate IoU (Intersection over Union)
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

// Merge two bounding boxes into one
BoundingBox merge_boxes(BoundingBox box1, BoundingBox box2)
{
    BoundingBox merged = {
        .x_min = (box1.x_min < box2.x_min) ? box1.x_min : box2.x_min,
        .y_min = (box1.y_min < box2.y_min) ? box1.y_min : box2.y_min,
        .x_max = (box1.x_max > box2.x_max) ? box1.x_max : box2.x_max,
        .y_max = (box1.y_max > box2.y_max) ? box1.y_max : box2.y_max};
    return merged;
}

// Filter out boxes that are too large
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

// Filter out boxes that are too small
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

// Filter boxes that touch frame edges
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

// Filter and merge overlapping boxes
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

// Convert bounding boxes to JSON string
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
        ESP_LOGE(detectorTag, "Failed to allocate memory for JSON string");
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
            // We're out of space; double size with reallocation
            size_t current_length = current_position - json_string;
            estimated_size *= 2;
            char *new_string = (char *)realloc(json_string, estimated_size);
            if (new_string == NULL)
            {
                ESP_LOGE(detectorTag, "Failed to reallocate memory for JSON string");
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

        // Add a comma if it's not the last box
        if (i < box_count - 1)
        {
            written = snprintf(current_position, remaining_size, ",");
            current_position += written;
            remaining_size -= written;
        }
    }

    // Close the JSON array
    written = snprintf(current_position, remaining_size, "]");
    current_position += written;

    // Optional: Trim memory to actual size
    size_t final_size = (current_position - json_string) + 1; // +1 for null terminator
    char *final_json = (char *)realloc(json_string, final_size);
    return final_json ? final_json : json_string; // Return final allocation or original
}

// Main motion detection function
bool detect_motion(const uint8_t *pixels, size_t width, size_t height, float threshold, time_t *detection_timestamp)
{
    if (!pixels || width == 0 || height == 0)
    {
        ESP_LOGE(detectorTag, "Invalid grayscale input");
        return false;
    }

    // Initialize background if needed
    if (!bg_model.background || bg_model.width != width || bg_model.height != height)
    {
        initialize_background_model(width, height);
    }

    update_background_model(pixels, width, height);

    // Normal background update

    if (!bg_model.initialized)
    {
        return false;
    }
    // Declare local variables
    size_t max_boxes = 30;
    size_t box_count = 0;
    BoundingBox *boxes = (BoundingBox *)malloc(max_boxes * sizeof(BoundingBox));
    if (!boxes)
    {
        ESP_LOGE(detectorTag, "Failed to allocate memory for bounding boxes");
        return false;
    }

    // Allocate memory for tracking changed pixels
    size_t *pixel_x = malloc(width * height * sizeof(size_t));
    size_t *pixel_y = malloc(width * height * sizeof(size_t));

    if (!pixel_x || !pixel_y)
    {
        ESP_LOGE(detectorTag, "Failed to allocate memory for pixel tracking");
        free(boxes);
        free(pixel_x);
        free(pixel_y);
        return false;
    }

    int changed_pixels = 0;
    size_t total_pixels = width * height;

    // Track positions of changed pixels
    for (size_t i = 0; i < total_pixels; i++)
    {
        uint8_t cur = pixels[i];
        if (abs(bg_model.background[i] - cur) > threshold)
        {
            changed_pixels++;
            size_t x = i % width;
            size_t y = i / width;
            pixel_x[changed_pixels - 1] = x;
            pixel_y[changed_pixels - 1] = y;
        }
    }

    // Early exit if very few pixels changed
    if (changed_pixels < MIN_COMPONENT_PIXELS)
    {
        free(boxes);
        free(pixel_x);
        free(pixel_y);
        return false;
    }

    // Create bounding boxes from clusters of changed pixels using Connected-Component Labeling
    int *labels = (int *)calloc(width * height, sizeof(int));
    if (!labels)
    {
        ESP_LOGE(detectorTag, "Failed to allocate memory for CCL labels");
        free(boxes);
        free(pixel_x);
        free(pixel_y);
        return false;
    }

    Component components[MAX_COMPONENTS] = {0};
    int next_label = 1;

    // Process each changed pixel
    for (size_t i = 0; i < changed_pixels; i++)
    {
        size_t x = pixel_x[i];
        size_t y = pixel_y[i];
        size_t idx = y * width + x;

        // Skip if this pixel is already labeled
        if (labels[idx] != 0)
            continue;

        // Skip if we've reached component limit
        if (next_label >= MAX_COMPONENTS)
            break;

        // Initialize new component
        Component comp = {
            .label = next_label,
            .x_min = x,
            .y_min = y,
            .x_max = x,
            .y_max = y,
            .pixel_count = 0};

        // BFS to find connected pixels in this component
        size_t queue_capacity = width * height;
        size_t *queue_x = (size_t *)malloc(queue_capacity * sizeof(size_t));
        size_t *queue_y = (size_t *)malloc(queue_capacity * sizeof(size_t));

        if (!queue_x || !queue_y)
        {
            ESP_LOGE(detectorTag, "Failed to allocate memory for BFS queue");
            free(labels);
            free(boxes);
            free(pixel_x);
            free(pixel_y);
            if (queue_x)
                free(queue_x);
            if (queue_y)
                free(queue_y);
            return false;
        }

        size_t q_start = 0, q_end = 0;

        queue_x[q_end] = x;
        queue_y[q_end++] = y;
        labels[idx] = next_label;

        while (q_start < q_end)
        {
            size_t cx = queue_x[q_start];
            size_t cy = queue_y[q_start++];
            comp.pixel_count++;

            // Update bounding box limits
            if (cx < comp.x_min)
                comp.x_min = cx;
            if (cy < comp.y_min)
                comp.y_min = cy;
            if (cx > comp.x_max)
                comp.x_max = cx;
            if (cy > comp.y_max)
                comp.y_max = cy;

            // Check 8-connected neighborhood
            for (int dy = -NEIGHBORHOOD; dy <= NEIGHBORHOOD; dy++)
            {
                for (int dx = -NEIGHBORHOOD; dx <= NEIGHBORHOOD; dx++)
                {
                    if (dx == 0 && dy == 0)
                        continue;

                    int nx = (int)cx + dx;
                    int ny = (int)cy + dy;

                    // Skip out-of-bounds pixels
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                        continue;

                    size_t nidx = ny * width + nx;

                    // Check if this neighboring pixel is also changed
                    uint8_t neighbor_cur = pixels[nidx];
                    bool neighbor_changed = abs(bg_model.background[nidx] - neighbor_cur) > threshold;

                    if (neighbor_changed && labels[nidx] == 0)
                    {
                        labels[nidx] = next_label;
                        queue_x[q_end] = nx;
                        queue_y[q_end++] = ny;
                    }
                }
            }
        }

        // Filter out small components immediately (noise)
        if (comp.pixel_count >= MIN_COMPONENT_PIXELS)
        {
            components[next_label - 1] = comp;
            next_label++;
        }
        else
        {
            // Reset labels for small component
            for (size_t k = 0; k < q_end; k++)
            {
                labels[queue_y[k] * width + queue_x[k]] = 0;
            }
        }

        free(queue_x);
        free(queue_y);
    }

    // Now create bounding boxes from detected components
    box_count = 0;
    for (int i = 0; i < next_label - 1 && box_count < max_boxes; i++)
    {
        boxes[box_count++] = (BoundingBox){
            .x_min = components[i].x_min,
            .y_min = components[i].y_min,
            .x_max = components[i].x_max,
            .y_max = components[i].y_max};
    }

    free(labels);
    free(pixel_x);
    free(pixel_y);

    // Apply filtering to bounding boxes
    if (box_count > 0)
    {
        // Apply various filters to refine the detection
        filter_and_merge_boxes(boxes, &box_count, IOU_THRESHOLD, MAX_BOX_AREA);
        filter_small_boxes(boxes, &box_count, MIN_BOX_AREA);
        filter_edge_touching_boxes(boxes, &box_count, width, height);

        // Check if there are still any boxes after filtering and merging
        if (box_count > 0)
        {
            // Set detection timestamp if provided
            if (detection_timestamp != NULL)
            {
                *detection_timestamp = time(NULL);
            }

            ESP_LOGI(detectorTag, "Motion detected! Boxes after filtering: %zu", box_count);

            // Generate JSON string for detected boxes
            char *json_string = boxes_to_json(boxes, box_count);
            if (json_string != NULL)
            {
                // Create a sensor data structure for the queue
                sensor_data_t sensor_data = {
                    .timestamp = time(NULL),
                    .temperature = 0, // These will be set elsewhere
                    .humidity = 0,
                    .pressure = 0,
                    .bboxes = json_string,
                    .owns_bboxes = true // This instance owns the memory
                };

                ESP_LOGI(detectorTag, "JSON string length: %zu", strlen(json_string));
                ESP_LOGI(detectorTag, "JSON string: %s", json_string);

                // Send to queue if available
                if (sensor_data_queue != NULL)
                {
                    if (xQueueSend(sensor_data_queue, &sensor_data, pdMS_TO_TICKS(10)) != pdTRUE)
                    {
                        ESP_LOGE(detectorTag, "Failed to send data to the queue");
                        // Clean up if send fails
                        free(json_string);
                    }
                }
                else
                {
                    ESP_LOGE(detectorTag, "sensor_data_queue is NULL");
                    free(json_string);
                }

                free(boxes);
                return true;
            }
        }
    }

    free(boxes);  // Always free the memory used for the boxes
    return false; // Return false if no boxes are left
}

// Clean up resources
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

    ESP_LOGI(detectorTag, "Background model resources cleaned up");
}
