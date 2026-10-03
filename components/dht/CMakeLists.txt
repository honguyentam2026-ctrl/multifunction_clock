idf_component_register(
    SRCS "dht.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio freertos esp_timer esp_rom log
)