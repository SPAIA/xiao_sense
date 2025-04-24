# Camera and SD Card FreeRTOS Project

## Overview

This project implements an ESP32-based IoT system that captures images when motion is detected, collects environmental data (temperature, humidity, pressure), stores data locally on an SD card, and uploads data to a cloud server.

## Documentation

Comprehensive documentation is available to help understand the system architecture and components:

- [Documentation Index](documentation_index.md) - Central hub for all documentation
- [Project Overview](project_overview.md) - High-level overview of the entire project

### Component Guides

Detailed guides for each major component:

- [Upload Manager Guide](upload_manager_guide.md) - Data upload coordination
- [Camera Interface Guide](camera_interface_guide.md) - Camera configuration and image capture
- [Motion Detector Guide](motion_detector_guide.md) - Motion detection system
- [SD Card Interface Guide](sdcard_interface_guide.md) - Local storage system
- [WiFi Interface Guide](wifi_interface_guide.md) - Network connectivity

## Key Features

- **Motion-triggered Photography**: Captures high-resolution images when motion is detected
- **Environmental Monitoring**: Collects temperature, humidity, and pressure data
- **Local Storage**: Stores all data on an SD card in an organized directory structure
- **Cloud Upload**: Uploads data to a cloud server in real-time or at scheduled intervals
- **Robust Connectivity**: Handles WiFi connection management and reconnection
- **Component-based Architecture**: Modular design with clear separation of concerns

## System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        Main Application                      │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────┼─────────────────────────────────┐
│                           │                                  │
│  ┌─────────────────┐  ┌───▼───────────┐  ┌────────────────┐ │
│  │ Sensor          │  │ Upload Manager │  │ Storage        │ │
│  │ Interfaces      │◄─┼─────┬─────────┼──┤ Interfaces     │ │
│  │ - Camera        │  │     │         │  │ - SD Card      │ │
│  │ - AHT (Temp/Hum)│  │     │         │  └────────────────┘ │
│  │ - BMP280        │  │     │         │                     │
│  └─────────────────┘  │     │         │  ┌────────────────┐ │
│                       │     │         │  │ Connectivity    │ │
│  ┌─────────────────┐  │     │         │  │ - WiFi         │ │
│  │ Processing      │  │     │         │  └────────┬───────┘ │
│  │ - Motion        │◄─┼─────┘         │          │          │
│  │   Detection     │  │               │          │          │
│  └─────────────────┘  └───────────────┘          ▼          │
│                                           ┌────────────────┐ │
│                                           │ File Upload    │ │
│                                           │ - HTTP Client  │ │
│                                           └────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

## Upload System

The upload system can operate in two modes:

1. **Real-time mode** (default, interval = 0): Files are uploaded immediately when created
2. **Interval mode** (interval > 0): Files are queued and uploaded at scheduled intervals

The current configuration in `main.c` initializes the upload manager with real-time mode:
```c
upload_manager_init(0);
```

## Directory Structure

The system organizes data on the SD card in a structured format:

```
/sdcard               # Mount point
  /spaia              # Main application directory
    /images           # Camera images
    /logs             # System logs
    /data             # Sensor data (CSV files)
    /config           # Configuration files
```

## Building and Development

### Building the Project

This project uses the ESP-IDF framework and can be built using the ESP-IDF VS Code Extension:

1. **Install Prerequisites**:
   - Install [Visual Studio Code](https://code.visualstudio.com/)
   - Install the [ESP-IDF VS Code Extension](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension)
   - Follow the extension's setup instructions to install ESP-IDF

2. **Build and Flash**:
   - Open the project folder in VS Code
   - Use the ESP-IDF extension commands from the VS Code command palette:
     - `ESP-IDF: Build project` to compile
     - `ESP-IDF: Flash device` to flash to the ESP32
     - `ESP-IDF: Monitor device` to view serial output

3. **Alternatively**, you can use the ESP-IDF extension toolbar buttons for build, flash, and monitor operations.

### Development Guidelines

When extending or modifying the system, consider:

1. **Component Integration**: How new features integrate with existing components
2. **Task Management**: Whether new FreeRTOS tasks are needed
3. **Resource Usage**: Memory, CPU, and power implications
4. **Error Handling**: Robust error handling and recovery
5. **Configuration**: How features can be configured

## Troubleshooting

Common issues and solutions are documented in the component guides. For general troubleshooting:

1. Check the ESP32 logs for error messages
2. Verify hardware connections
3. Ensure WiFi connectivity
4. Check SD card mounting and file system
5. Verify server connectivity for uploads

## License

[Add license information here]

Applied a patch to the camera from here:
https://github.com/remibert/pycameresp/blob/main/patch/S3/c/esp32-camera/driver/cam_hal.c
This keeps memory allocated between de-init's to help prevent heap fragmentation.

