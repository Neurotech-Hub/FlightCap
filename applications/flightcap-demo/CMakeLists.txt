cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(flightcap_demo LANGUAGES C)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/lis2dh12
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/vl53l4cd
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/flightcap_hw
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/vbatt
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/spi_nor
)

target_sources(app PRIVATE
    src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/lis2dh12/lis2dh12_zephyr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/lis2dh12/lis2dh12_reg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/vl53l4cd/vl53l4cd_filter.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/vl53l4cd/vl53l0x_zephyr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/vbatt/vbatt_zephyr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/spi_nor/spi_nor_zephyr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/flightcap_hw/flightcap_hw.c
)
