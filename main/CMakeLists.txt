idf_component_register(
    SRCS
        "main.c"
        "audio.cpp"
        "battery.c"
        "ble_setup.c"
        "boot_logo.c"
        "board.cpp"
        "button.c"
        "display.c"
        "handwriting.c"
        "imu.c"
        "render.c"
        "rtc_clock.c"
        "sim.c"
        "settings.c"
        "touch.cpp"
        "ui_font.c"
        "ui_font_source_han_20.c"
        "ui_font_source_han_24.c"
        "ui_font_source_han_32.c"
        "ui_font_source_han_40.c"
        "ui_font_source_han_48.c"
        "ui_font_source_han_64.c"
        "wifi_setup.c"
        "weather.c"
    INCLUDE_DIRS "."
    REQUIRES
        BMI270_BMM150_Sensor
        M5IOE1
        nvs_flash
        bt
        esp_event
        esp_http_server
        esp_http_client
        esp_netif
        esp_wifi
        lwip
        json
)

# The solver's inner loops are pure floating-point math and do not rely on
# strict NaN/Inf behaviour. These flags are inherited from upstream FluidBox.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffast-math)

# A successful C compile must never hide a stale subset behind the replacement
# box. This dependency scans runtime strings on every build and fails early if
# either shared Chinese/English UI font is missing a codepoint.
add_custom_target(ui_font_coverage_check ALL
    COMMAND ${PYTHON} ${CMAKE_CURRENT_LIST_DIR}/../tools/check_ui_font_coverage.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    VERBATIM
)
add_dependencies(${COMPONENT_LIB} ui_font_coverage_check)
