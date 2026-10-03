idf_component_register(
    SRCS "main.c" "app_queues.c" "task_button.c"
         "task_clock.c" "task_temp.c" "task_mode.c" "task_display.c"
         "task_alarm.c" "storage.c" "diag.c"
    INCLUDE_DIRS "."
    REQUIRES i2c_bus ds3231 dht ssd1306 font5x7 esp_driver_gpio freertos log nvs_flash esp_timer heap
)