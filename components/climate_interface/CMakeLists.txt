idf_component_register(SRCS "climate_interface.c"
    INCLUDE_DIRS "include"
    REQUIRES i2cdev
    PRIV_REQUIRES bmp280 sdcard_interface esp32-camera
)
