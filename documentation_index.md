# Camera and SD Card FreeRTOS Project Documentation

This index provides links to all documentation for the Camera and SD Card FreeRTOS project, designed to help AI coding agents quickly find relevant information.

## Project Overview

- [Project Overview](project_overview.md) - High-level overview of the entire project, its architecture, and components

## Component Guides

### Core Components

- [Upload Manager Guide](upload_manager_guide.md) - Detailed information about the upload manager component
- [Camera Interface Guide](camera_interface_guide.md) - Guide to the camera initialization, configuration, and image capture
- [Motion Detector Guide](motion_detector_guide.md) - Information about the motion detection system
- [SD Card Interface Guide](sdcard_interface_guide.md) - Guide to the local storage system
- [WiFi Interface Guide](wifi_interface_guide.md) - Information about the network connectivity component

### Sensor Components

- AHT Interface - Temperature and humidity sensor interface
- BMP280 - Pressure sensor interface
- Climate Interface - Combined environmental sensor interface

### Utility Components

- File Upload - HTTP(S) file upload implementation
- I2C Device - I2C communication utilities
- ESP-IDF Library Helpers - Utility functions for ESP-IDF

## Key Concepts

### Data Flow

1. **Sensor Data Collection**:
   - Camera captures frames continuously for motion detection
   - Temperature/humidity sensors collect data at regular intervals

2. **Event Detection**:
   - Motion detector analyzes camera frames
   - When motion is detected, a high-resolution photo is captured

3. **Data Storage**:
   - Sensor data and images are stored on the SD card
   - Data is organized in a structured format (CSV files for sensor data)

4. **Data Upload**:
   - Upload Manager determines when to upload data based on configuration:
     - Real-time mode (interval = 0): Upload immediately when new data is available
     - Interval mode (interval > 0): Upload at scheduled intervals
   - File Upload component handles the actual HTTP(S) transfer

### Task Structure

The system uses multiple FreeRTOS tasks to handle different responsibilities:

- **Motion Detection Task**: Continuously analyzes camera frames
- **AHT Reading Task**: Periodically reads temperature and humidity
- **Upload Task**: Manages the upload schedule and triggers uploads
- **File Upload Task**: Handles the actual file transfer operations

### Configuration

Key configuration is managed through:
- Header files (e.g., `wifi_config.h`, `camera_config.h`)
- Runtime configuration via the Upload Manager interface

## Quick Reference

### Upload Manager Modes

The upload system can operate in two modes:
1. **Real-time mode** (default, interval = 0): Files are uploaded immediately when created
2. **Interval mode** (interval > 0): Files are queued and uploaded at scheduled intervals

### Camera Modes

The camera operates in two primary modes:
1. **Motion Detection Mode**: Low-resolution grayscale for efficient processing
2. **Photo Capture Mode**: High-resolution JPEG for quality images

### Directory Structure

```
/sdcard               # Mount point
  /spaia              # Main application directory
    /images           # Camera images
    /logs             # System logs
    /data             # Sensor data (CSV files)
    /config           # Configuration files
```

## Development Guides

### Adding New Features

When adding new features to the system, consider:

1. **Component Integration**: How the new feature integrates with existing components
2. **Task Management**: Whether a new FreeRTOS task is needed
3. **Resource Usage**: Memory, CPU, and power implications
4. **Error Handling**: Robust error handling and recovery
5. **Configuration**: How the feature can be configured

### Debugging Tips

1. **Log Levels**: Use appropriate ESP log levels (ERROR, WARN, INFO, DEBUG)
2. **Task Monitoring**: Monitor task stack usage and CPU time
3. **Memory Analysis**: Check for memory leaks and fragmentation
4. **Power Profiling**: Measure power consumption of different components

## Common Issues and Solutions

1. **WiFi Connectivity Issues**
   - Check signal strength and AP availability
   - Implement more aggressive reconnection strategies

2. **Camera Problems**
   - Verify hardware connections
   - Adjust camera settings for different lighting conditions

3. **SD Card Errors**
   - Check card format and connections
   - Implement file system recovery mechanisms

4. **Upload Failures**
   - Verify server connectivity and authentication
   - Implement retry mechanisms with exponential backoff
