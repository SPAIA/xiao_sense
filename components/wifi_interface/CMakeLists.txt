idf_component_register(SRCS "wifi_interface.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_wifi esp_event esp_netif json)
