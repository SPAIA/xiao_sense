idf_component_register(SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES camera_interface sdcard_interface wifi_interface file_upload aht_interface motion_detector upload_manager)
