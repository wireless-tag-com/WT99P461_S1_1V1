
idf_component_register(
    SRCS "wt99p461_s1_board.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_sdmmc vfs fatfs esp_eth esp_netif esp_event led_strip
    PRIV_REQUIRES esp_hal_lcd esp_lcd_jd9165 esp_driver_ledc esp_lcd usb spiffs
)

set(WT99_SD_TEST 0)
target_compile_definitions(${COMPONENT_LIB} PUBLIC SD_TEST=${WT99_SD_TEST})
