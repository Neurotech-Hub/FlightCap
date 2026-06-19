cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(flightcap_axy LANGUAGES C)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/lis2dh12
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/vbatt
)

target_sources(app PRIVATE
    src/main.c
    src/lis2dh12_zephyr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/lis2dh12/lis2dh12_reg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/vbatt/vbatt_zephyr.c
)
