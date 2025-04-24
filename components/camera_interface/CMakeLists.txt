idf_component_register(SRCS "camera_interface.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
    PRIV_REQUIRES esp32-camera sdcard_interface motion_detector freertos upload_manager
)
