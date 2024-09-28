# In your project's root CMakeLists.txt
idf_component_register(SRCS "file_upload.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client freertos sdcard_interface wifi_interface esp32-camera
)
