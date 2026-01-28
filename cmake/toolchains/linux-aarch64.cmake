# Cross-compile toolchain for Raspberry Pi 4 (aarch64)
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-aarch64.cmake ..

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Cross-compiler (install: apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# RPi4-specific optimizations
set(CMAKE_C_FLAGS_RELEASE "-O2 -mcpu=cortex-a72" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -mcpu=cortex-a72" CACHE STRING "" FORCE)
