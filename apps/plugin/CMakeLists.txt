cmake_minimum_required(VERSION 3.16)

# Soluna CoreAudio AudioServerPlugIn
# Builds as a .driver bundle installed to /Library/Audio/Plug-Ins/HAL/

add_library(Soluna MODULE
    SolunaPlugin.mm
)

set_target_properties(Soluna PROPERTIES
    BUNDLE                      TRUE
    BUNDLE_EXTENSION            "driver"
    OUTPUT_NAME                 "Soluna"
    MACOSX_BUNDLE_INFO_PLIST    "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
)

target_include_directories(Soluna PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

find_library(COREAUDIO_FW   CoreAudio   REQUIRED)
find_library(COREFOUNDATION_FW CoreFoundation REQUIRED)

target_link_libraries(Soluna PRIVATE
    ${COREAUDIO_FW}
    ${COREFOUNDATION_FW}
)

target_compile_options(Soluna PRIVATE
    -Wall -Wextra
    $<$<CONFIG:Release>:-O2>
)

# Install target (requires sudo)
# MODULE libraries need LIBRARY DESTINATION; BUNDLE is for macOS app bundles (.app)
install(TARGETS Soluna
    LIBRARY DESTINATION "/Library/Audio/Plug-Ins/HAL"
)
