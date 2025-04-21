# In your project's root CMakeLists.txt
idf_component_register(SRCS "upload_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client freertos sdcard_interface wifi_interface esp-tls file_upload sdcard_interface
)
