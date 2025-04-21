idf_component_register(SRCS "sdcard_interface.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp32-camera fatfs sdmmc file_upload upload_manager
)
