# Motion Detector Component Guide

This document provides detailed information about the Motion Detector component, which is responsible for analyzing camera frames to detect motion and trigger image capture.

## Overview

The Motion Detector component:
- Analyzes camera frames to detect changes between frames
- Uses background subtraction and image processing techniques
- Triggers high-resolution photo capture when motion is detected
- Works in conjunction with the Camera Interface and Upload Manager

## Architecture

The Motion Detector integrates with the Camera Interface and operates as part of the motion detection task:

```
┌─────────────────────────────────────────────────────────────┐
│                    Motion Detection System                   │
│                                                             │
│  ┌─────────────────┐      ┌─────────────────────────────┐  │
│  │ Camera Interface│      │ Motion Detection Task       │  │
│  │ - Frame         │──┬──▶│ - Runs continuously         │  │
│  │   acquisition   │  │   │ - Analyzes frames           │  │
│  │ - Camera config │  │   │ - Detects motion            │  │
│  └─────────────────┘  │   └─────────────────┬───────────┘  │
│                       │                     │               │
│                       │                     │ (Motion       │
│                       │                     │  detected)    │
│                       │                     │               │
│                       │                     ▼               │
│  ┌─────────────────┐  │   ┌─────────────────────────────┐  │
│  │ Background      │  │   │ High-Resolution Capture     │  │
│  │ Model           │◀─┘   │ - Switches camera mode      │  │
│  │ - Reference     │      │ - Takes high-res photo      │  │
│  │   frame         │      │ - Notifies upload manager   │  │
│  │ - Thresholds    │      └─────────────┬───────────────┘  │
│  └─────────────────┘                    │                  │
│                                         │                  │
└─────────────────────────────────────────┼──────────────────┘
                                          │
                                          ▼
                             ┌─────────────────────────────┐
                             │ Upload Manager              │
                             │ - Handles file upload       │
                             └─────────────────────────────┘
```

## Key Components

### Background Model
- Maintains a reference frame for comparison
- Updates adaptively to account for gradual lighting changes
- Uses grayscale images to focus on luminance changes

### Motion Detection Algorithm
- Compares current frame with background model
- Applies thresholding to identify significant changes
- Uses noise reduction and filtering techniques
- Calculates motion metrics based on pixel differences

### Motion Detection Task
- Runs as a dedicated FreeRTOS task
- Continuously acquires and processes frames
- Coordinates with the camera interface via semaphore
- Triggers high-resolution capture when motion is detected

## Implementation Details

### Motion Detection Process

1. **Frame Acquisition**:
   - Acquire a frame from the camera in grayscale format
   - Lower resolution (typically QVGA 320x240) for efficiency

2. **Frame Processing**:
   - Compare with background model
   - Apply thresholding to identify changed pixels
   - Filter out noise and small movements

3. **Motion Analysis**:
   - Calculate the percentage or magnitude of change
   - Compare against threshold to determine if motion occurred
   - Timestamp the motion event

4. **Action Triggering**:
   - When motion is detected, trigger high-resolution photo capture
   - Switch camera to high-resolution mode (typically SXGA)
   - Capture JPEG image
   - Switch back to motion detection mode

### Key Functions

#### Motion Detection

```c
bool detect_motion(camera_fb_t *frame, float threshold, time_t *timestamp);
```
- Analyzes a camera frame for motion
- Returns true if motion is detected
- Sets the timestamp of when motion was detected
- Uses the specified threshold for sensitivity

#### Background Model Initialization

```c
void initialize_background_model(int width, int height);
```
- Sets up the initial background model
- Allocates memory for the background frame
- Configures dimensions based on camera resolution

#### High-Resolution Capture

```c
esp_err_t takeHighResPhoto(time_t timestamp);
```
- Switches camera to high-resolution mode
- Captures a photo
- Notifies the upload manager
- Switches back to motion detection mode

## Integration with Other Components

### Camera Interface
The motion detector works closely with the camera interface:
- Uses the camera semaphore for thread-safe access
- Configures the camera for different modes (detection vs. capture)
- Processes frames acquired from the camera

```c
// Example from motion_detection_task
if (xSemaphoreTake(camera_semaphore, portMAX_DELAY) == pdTRUE) {
    camera_fb_t *frame = esp_camera_fb_get();
    if (frame) {
        if (detect_motion(frame, motion_threshold, &motion_timestamp)) {
            ESP_LOGI(cameraTag, "Motion detected!");
            esp_camera_fb_return(frame);
            xSemaphoreGive(camera_semaphore);

            if (takeHighResPhoto(motion_timestamp) != ESP_OK) {
                ESP_LOGE(cameraTag, "Failed to take high-res photo");
            }
            continue;
        }
        esp_camera_fb_return(frame);
    }
    xSemaphoreGive(camera_semaphore);
}
```

### Upload Manager
When motion is detected and a high-resolution photo is captured:
- The photo is passed to the upload manager
- In real-time mode, this triggers an immediate upload
- In interval mode, the file is queued for later upload

```c
// Example from takeHighResPhoto
if (upload_manager_notify_new_file(pic) != ESP_OK) {
    ESP_LOGE(cameraTag, "Failed to save image");
    ret = ESP_FAIL;
}
```

## Configuration

### Motion Sensitivity

The motion detection sensitivity can be adjusted by changing the threshold value:

```c
float motion_threshold = 50; // Default value
```

- Lower values increase sensitivity (more motion events)
- Higher values decrease sensitivity (fewer motion events)
- The optimal value depends on the environment and use case

### Camera Settings

The motion detector uses different camera configurations:
- Low-resolution grayscale for motion detection (efficient processing)
- High-resolution JPEG for photo capture (better image quality)

```c
// Motion detection mode
highres_config.frame_size = FRAMESIZE_QVGA;     // 320x240
highres_config.pixel_format = PIXFORMAT_GRAYSCALE;

// Photo capture mode
highres_config.frame_size = FRAMESIZE_SXGA;     // 1280x1024
highres_config.pixel_format = PIXFORMAT_JPEG;
```

## Performance Considerations

### CPU Usage
- Motion detection is computationally intensive
- The task runs at a lower priority to avoid impacting critical system functions
- Frame rate and resolution are balanced for efficient detection

### Memory Usage
- Grayscale images require less memory than color
- The background model requires additional memory
- High-resolution capture temporarily increases memory usage

### Power Consumption
- Continuous frame processing consumes significant power
- Consider using interval-based motion detection for battery-powered applications
- Optimize thresholds to reduce false positives

## Troubleshooting

### Common Issues

1. **False Positives**
   - Too many motion events in stable environments
   - Solution: Increase the motion threshold
   - Check for environmental factors (e.g., changing light conditions)

2. **Missed Motion Events**
   - Motion not being detected when expected
   - Solution: Decrease the motion threshold
   - Ensure camera is properly positioned and focused

3. **System Performance Issues**
   - System becoming sluggish during motion detection
   - Solution: Reduce frame resolution or processing frequency
   - Check for memory leaks in the motion detection loop

4. **Camera Mode Switching Problems**
   - Failures when switching between detection and capture modes
   - Solution: Ensure proper semaphore handling
   - Add delay between mode switches
   - Check for camera driver issues

## Extending the Motion Detector

### Possible Enhancements

1. **Region of Interest (ROI)**
   - Implement detection only in specific areas of the frame
   - Ignore motion in non-relevant areas

2. **Advanced Algorithms**
   - Implement more sophisticated motion detection algorithms
   - Consider optical flow or deep learning-based approaches

3. **Motion Classification**
   - Distinguish between different types of motion
   - Classify objects causing the motion (person, animal, vehicle)

4. **Adaptive Thresholds**
   - Automatically adjust thresholds based on environment
   - Implement day/night sensitivity adjustments
