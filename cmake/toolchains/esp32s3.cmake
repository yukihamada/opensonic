# ESP32-S3 cross-compile toolchain
# Note: For ESP32 firmware, use the ESP-IDF build system directly:
#   cd apps/esp32 && idf.py build
#
# This toolchain is for building the core library for ESP32 testing.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR xtensa)

# ESP-IDF toolchain (install via: idf_tools.py install)
set(CMAKE_C_COMPILER xtensa-esp32s3-elf-gcc)
set(CMAKE_CXX_COMPILER xtensa-esp32s3-elf-g++)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# ESP32 constraints
add_compile_definitions(SOLUNA_ESP32=1)
set(CMAKE_C_FLAGS "-mlongcalls -fno-exceptions" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "-mlongcalls -fno-exceptions -fno-rtti" CACHE STRING "" FORCE)
