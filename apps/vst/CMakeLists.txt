cmake_minimum_required(VERSION 3.16)

# ── Soluna VST3 Network Audio Receiver Plugin ──────────────────────────────
#
# Builds a VST3 plugin bundle that receives Soluna network audio
# (multicast 239.69.0.1:5004 by default, or P2P unicast) and outputs
# it to the DAW's audio bus.
#
# The plugin is self-contained: no dependency on soluna_core or the
# VST3 SDK. All required VST3 interfaces are defined inline.
#
# Build:
#   cmake -B build-vst -S apps/vst
#   cmake --build build-vst --config Release
#
# Or from the top-level CMake (when SOLUNA_BUILD_VST is ON):
#   cmake -B build -DSOLUNA_BUILD_VST=ON
#   cmake --build build --target soluna_vst
#
# Install (macOS):
#   cp -R build-vst/Soluna.vst3 ~/Library/Audio/Plug-Ins/VST3/
#
# Install (Linux):
#   cp -R build-vst/Soluna.vst3 ~/.vst3/
#
# Install (Windows):
#   copy build-vst\Release\Soluna.vst3 "%COMMONPROGRAMFILES%\VST3\"

project(soluna_vst
    VERSION 0.1.0
    DESCRIPTION "Soluna Network Audio Receiver — VST3 Plugin"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Sources ──────────────────────────────────────────────────────────────────

add_library(soluna_vst MODULE
    soluna_plugin.cpp
    vst3_wrapper.cpp
)

target_include_directories(soluna_vst PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ── Platform-specific configuration ─────────────────────────────────────────

if(APPLE)
    # macOS: Build as .vst3 bundle
    set_target_properties(soluna_vst PROPERTIES
        BUNDLE                    TRUE
        BUNDLE_EXTENSION          "vst3"
        OUTPUT_NAME               "Soluna"
        MACOSX_BUNDLE_GUI_IDENTIFIER "audio.soluna.vst3.receiver"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST  "${CMAKE_CURRENT_SOURCE_DIR}/Info.vst3.plist"
    )

    # Symbol visibility
    target_compile_options(soluna_vst PRIVATE
        -fvisibility=hidden
        -fvisibility-inlines-hidden
    )

    # Export only the VST3 entry points
    target_link_options(soluna_vst PRIVATE
        -exported_symbols_list ${CMAKE_CURRENT_SOURCE_DIR}/exported_symbols.txt
    )

    # Generate Info.plist if not present (CMake will generate a default)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Info.vst3.plist")
        set_target_properties(soluna_vst PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST ""
        )
    endif()

elseif(WIN32)
    # Windows: .vst3 is just a DLL with .vst3 extension
    set_target_properties(soluna_vst PROPERTIES
        OUTPUT_NAME "Soluna"
        SUFFIX      ".vst3"
    )
    target_link_libraries(soluna_vst PRIVATE ws2_32)

else()
    # Linux: .vst3 is a shared object inside a bundle directory structure
    set_target_properties(soluna_vst PROPERTIES
        OUTPUT_NAME "Soluna"
        PREFIX      ""
        SUFFIX      ".so"
    )
    target_link_libraries(soluna_vst PRIVATE pthread)

    # Post-build: create VST3 bundle directory structure
    # Linux VST3 bundles: Soluna.vst3/Contents/x86_64-linux/Soluna.so
    set(VST3_ARCH "x86_64-linux")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(VST3_ARCH "aarch64-linux")
    endif()

    add_custom_command(TARGET soluna_vst POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_BINARY_DIR}/Soluna.vst3/Contents/${VST3_ARCH}"
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:soluna_vst>"
            "${CMAKE_BINARY_DIR}/Soluna.vst3/Contents/${VST3_ARCH}/Soluna.so"
        COMMENT "Creating VST3 bundle: Soluna.vst3"
    )
endif()

# ── Compiler flags ───────────────────────────────────────────────────────────

target_compile_options(soluna_vst PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
        -Wall -Wextra -Wpedantic
        $<$<CONFIG:Release>:-O2 -DNDEBUG>
    >
    $<$<CXX_COMPILER_ID:MSVC>:
        /W4
        $<$<CONFIG:Release>:/O2 /DNDEBUG>
    >
)
