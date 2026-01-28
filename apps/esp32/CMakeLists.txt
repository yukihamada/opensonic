# Soluna ESP32 Firmware
# Requires ESP-IDF v5.x
# Build: idf.py build
# Flash: idf.py -p /dev/ttyUSB0 flash monitor

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(soluna_esp32)
