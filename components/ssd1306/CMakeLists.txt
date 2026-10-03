idf_component_register(
    SRCS "ssd1306.c"
    INCLUDE_DIRS "include"
    REQUIRES i2c_bus esp_driver_i2c log font5x7
)
