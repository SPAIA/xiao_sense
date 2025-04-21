# Camera and SD Card FreeRTOS Project Overview

This document provides a high-level overview of the Camera and SD Card FreeRTOS project, designed to help AI coding agents quickly understand the context of any given file.

## Project Purpose

This is an ESP32-based IoT system that:
- Captures images using a camera when motion is detected
- Collects environmental data (temperature, humidity, pressure)
- Stores data locally on an SD card
- Uploads data to a cloud server (real-time or on intervals)

## System Architecture

The project follows a component-based architecture using ESP-IDF and FreeRTOS:

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

## Key Components

### Main Application (`main/`)
- **main.c**: Entry point that initializes all subsystems and manages the application lifecycle
- Coordinates the initialization sequence and handles WiFi connectivity events

### Sensor Interfaces
- **Camera Interface** (`components/camera_interface/`):
  - Manages camera initialization, configuration, and image capture
  - Implements motion detection integration
  - Handles high-resolution photo capture when motion is detected

- **AHT Interface** (`components/aht_interface/`):
  - Interfaces with the AHT20 temperature and humidity sensor
  - Provides periodic readings through a dedicated task

- **Climate Interface** (`components/climate_interface/`):
  - Higher-level component that may combine multiple sensor readings

- **BMP280** (`components/bmp280/`):
  - Interfaces with the BMP280 pressure sensor

### Processing
- **Motion Detector** (`components/motion_detector/`):
  - Analyzes camera frames to detect motion
  - Triggers image capture when motion is detected

### Storage
- **SD Card Interface** (`components/sdcard_interface/`):
  - Manages SD card initialization and file operations
  - Provides functions for reading and writing files

### Connectivity
- **WiFi Interface** (`components/wifi_interface/`):
  - Manages WiFi connection and reconnection
  - Provides status callbacks for connection state changes

### Data Management
- **Upload Manager** (`components/upload_manager/`):
  - Coordinates data uploads to the server
  - Supports both real-time and interval-based upload modes
  - Acts as a central point for managing file uploads

- **File Upload** (`components/file_upload/`):
  - Handles the HTTP(S) communication for uploading files
  - Implements the actual file transfer logic

## Data Flow

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

## Key Interfaces and Configuration

### Upload Manager
- **upload_manager_init(uint32_t upload_interval_seconds)**:
  - Initializes the upload manager with specified interval
  - 0 = real-time uploads, >0 = interval-based uploads in seconds

- **upload_manager_notify_new_file(const char *filename)**:
  - Notifies the upload manager about a new file
  - In real-time mode, triggers immediate upload

### Camera Interface
- **initialize_camera()**:
  - Sets up the camera with appropriate configuration
  - Detects sensor type and optimizes settings

- **takeHighResPhoto(time_t timestamp)**:
  - Captures a high-resolution photo when motion is detected

### WiFi Interface
- **initialize_wifi()**:
  - Connects to configured WiFi network

- **register_wifi_status_callback(wifi_status_callback_t callback)**:
  - Registers a callback for WiFi connection state changes

## FreeRTOS Task Structure

The system uses multiple FreeRTOS tasks to handle different responsibilities:

- **Motion Detection Task**: Continuously analyzes camera frames
- **AHT Reading Task**: Periodically reads temperature and humidity
- **Upload Task**: Manages the upload schedule and triggers uploads
- **File Upload Task**: Handles the actual file transfer operations

## Configuration

Key configuration is managed through:
- Header files (e.g., `wifi_config.h`, `camera_config.h`)
- Runtime configuration via the Upload Manager interface

## Understanding the Upload System

The upload system can operate in two modes:
1. **Real-time mode** (default, interval = 0): Files are uploaded immediately when created
2. **Interval mode** (interval > 0): Files are queued and uploaded at scheduled intervals

The current configuration in `main.c` initializes the upload manager with real-time mode:
```c
upload_manager_init(0);
```
