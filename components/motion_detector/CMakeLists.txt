idf_component_register(SRCS "motion_detector.c"
    INCLUDE_DIRS "include"
    REQUIRES esp32-camera sdcard_interface
)
