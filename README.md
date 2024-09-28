Heavily based on the information [here](https://wiki.seeedstudio.com/xiao-esp32s3-freertos/) and the hard work shared [here](https://github.com/Priyanshu0901/Camera-and-SdCard-FreeRTOS)

| Supported Targets | Xiao ESP32-S3 (Sense)  |

# Configure the project
Open the project configuration menu (idf.py menuconfig).

In the Example Configuration menu:

Set the Wi-Fi configuration.
Set WiFi SSID.
Set WiFi Password.

You fursther need to enable the option "Support for external, SPI-connected RAM" annd change "Mode (QUAD/OCT) of SPI RAM chip in use" to "octalmode PSRAM"

Camera and Sdcard mapped for Xiao ESP32-S3 (Sense)   

# Pin Map

SDcard and Camera pin mapping can be found under their respective components directory -> include -> config.h

# Sdkconfig

When using your own board make sure to activate PSRAM support in the menuconfig. Please ensure the PSRAM mode is set to "octal"

# For More Info

[XIAO ESP32S3(Sense) FreeRTOS](https://wiki.seeedstudio.com/xiao-esp32s3-freertos/)
