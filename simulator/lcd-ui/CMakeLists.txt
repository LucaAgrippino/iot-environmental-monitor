cmake_minimum_required(VERSION 3.16)
project(lcd_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Path to the shared lv_conf.h (firmware authoritative copy)
set(LV_CONF_PATH
    ${CMAKE_SOURCE_DIR}/../../firmware/field-device/middleware/graphics_library/lv_conf.h
    CACHE STRING "Path to lv_conf.h")

add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
add_compile_definitions(LV_CONF_PATH=${LV_CONF_PATH})
add_compile_definitions(SIMULATOR)   # suppresses FreeRTOS includes in lcd_ui.c

# Override the embedded 32 KB heap limit — the simulator has plenty of RAM.
add_compile_definitions(LV_MEM_SIZE=524288)   # 512 KB

# LVGL submodule
add_subdirectory(${CMAKE_SOURCE_DIR}/../../vendor/lvgl lvgl_build)

# SDL2
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

# Firmware source root shortcuts
set(FW_ROOT ${CMAKE_SOURCE_DIR}/../../firmware/field-device)
set(APP_ROOT ${FW_ROOT}/application)
set(MW_ROOT  ${FW_ROOT}/middleware)

add_executable(lcd_sim
    main.c
    sdl_backend.c
    sim_providers.c

    # LcdUi application module
    ${APP_ROOT}/lcd_ui/lcd_ui.c
    ${APP_ROOT}/lcd_ui/theme.c
    ${APP_ROOT}/lcd_ui/widgets/tab_bar.c
    ${APP_ROOT}/lcd_ui/widgets/header_bar.c
    ${APP_ROOT}/lcd_ui/widgets/footer_bar.c
    ${APP_ROOT}/lcd_ui/widgets/status_pill.c
    ${APP_ROOT}/lcd_ui/widgets/sensor_card.c
)

target_include_directories(lcd_sim PRIVATE
    # LVGL config header
    ${MW_ROOT}/graphics_library

    # Firmware middleware headers (time_provider/, etc.)
    ${MW_ROOT}

    # Firmware application headers (sensor_service/, alarm_service/, etc.)
    ${APP_ROOT}

    # SDL2 platform headers
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(lcd_sim PRIVATE lvgl ${SDL2_LIBRARIES})
target_compile_options(lcd_sim PRIVATE ${SDL2_CFLAGS_OTHER})
