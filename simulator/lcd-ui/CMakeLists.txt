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

# LVGL submodule
add_subdirectory(${CMAKE_SOURCE_DIR}/../../vendor/lvgl lvgl_build)

# SDL2
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

# lv_drivers SDL2 backend (vendored as a small helper to avoid pulling
# the whole lv_drivers repo). We will inline the two files we need.
add_executable(lcd_sim
    main.c
    sdl_backend.c
)

target_include_directories(lcd_sim PRIVATE
    ${CMAKE_SOURCE_DIR}/../../firmware/field-device/middleware/graphics_library
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(lcd_sim PRIVATE lvgl ${SDL2_LIBRARIES})
target_compile_options(lcd_sim PRIVATE ${SDL2_CFLAGS_OTHER})
