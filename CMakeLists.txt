cmake_minimum_required(VERSION 3.16)
project(soluna
    VERSION 0.1.0
    DESCRIPTION "Soluna — Open Network Audio System"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
include(cmake/options.cmake)

# Platform detection
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(ANDROID)
        set(OS_ANDROID TRUE)
    else()
        set(OS_LINUX TRUE)
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(OS_MACOS TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(OS_IOS TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(OS_WINDOWS TRUE)
endif()

# Third-party dependencies
if(SOLUNA_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif()

# Core library
add_library(soluna_core STATIC
    # PAL - Audio
    $<$<BOOL:${OS_LINUX}>:src/pal/audio/alsa.cpp>
    $<$<BOOL:${OS_MACOS}>:src/pal/audio/coreaudio.mm>
    $<$<BOOL:${OS_IOS}>:src/pal/audio/coreaudio.mm>
    $<$<BOOL:${OS_ANDROID}>:src/pal/audio/aaudio.cpp>
    $<$<BOOL:${OS_WINDOWS}>:src/pal/audio/wasapi.cpp>
    # PAL - Net (POSIX for Linux, macOS, iOS, Android)
    $<$<OR:$<BOOL:${OS_LINUX}>,$<BOOL:${OS_MACOS}>,$<BOOL:${OS_IOS}>,$<BOOL:${OS_ANDROID}>>:src/pal/net/socket_posix.cpp>
    $<$<BOOL:${OS_WINDOWS}>:src/pal/net/socket_win32.cpp>
    # PAL - Time (POSIX for Linux, macOS, iOS, Android)
    $<$<OR:$<BOOL:${OS_LINUX}>,$<BOOL:${OS_MACOS}>,$<BOOL:${OS_IOS}>,$<BOOL:${OS_ANDROID}>>:src/pal/time/clock_posix.cpp>
    $<$<BOOL:${OS_WINDOWS}>:src/pal/time/clock_win32.cpp>
    # PAL - Thread (POSIX for Linux, macOS, iOS, Android)
    $<$<OR:$<BOOL:${OS_LINUX}>,$<BOOL:${OS_MACOS}>,$<BOOL:${OS_IOS}>,$<BOOL:${OS_ANDROID}>>:src/pal/thread/thread_posix.cpp>
    $<$<BOOL:${OS_WINDOWS}>:src/pal/thread/thread_win32.cpp>
    # Transport
    src/transport/rtp_tx.cpp
    src/transport/rtp_rx.cpp
    src/transport/ostp_header.cpp
    src/transport/packet_scheduler.cpp
    # Sync
    src/sync/ptp_messages.cpp
    src/sync/ptp_engine.cpp
    src/sync/bmca.cpp
    src/sync/clock_servo.cpp
    src/sync/adaptive_pll.cpp
    # Pipeline
    src/pipeline/ring_buffer.cpp
    src/pipeline/audio_pipeline.cpp
    src/pipeline/format_converter.cpp
    src/pipeline/playout_buffer.cpp
    src/pipeline/dsp_plugin.cpp
    src/pipeline/dsp_chain.cpp
    # Control
    src/control/discovery.cpp
    src/control/session_manager.cpp
    src/control/routing_matrix.cpp
    src/control/control_protocol.cpp
    src/control/rest_api.cpp
    src/control/websocket_server.cpp
    src/control/auto_router.cpp
    # WiFi
    src/wifi/jitter_buffer.cpp
    src/wifi/fec_encoder.cpp
    src/wifi/fec_decoder.cpp
    src/wifi/adaptation_controller.cpp
    # Codec
    src/codec/opus_wrapper.cpp
    src/codec/codec_factory.cpp
    $<$<BOOL:${SOLUNA_ENABLE_AAC}>:src/codec/aac_wrapper.cpp>
    $<$<BOOL:${SOLUNA_ENABLE_FLAC}>:src/codec/flac_wrapper.cpp>
    # Core
    src/core/error.cpp
    # Config
    src/config/yaml_parser.cpp
    src/config/config_loader.cpp
    # Metrics
    src/metrics/registry.cpp
    src/metrics/exporter.cpp
    # Security
    src/security/auth.cpp
    src/security/acl.cpp
    src/security/token.cpp
    # Audit
    src/audit/audit_log.cpp
    # AES67 (conditional)
    $<$<BOOL:${SOLUNA_ENABLE_AES67}>:src/transport/aes67.cpp>
    # DTLS (always compiled — has internal #ifdef guards for stub)
    src/transport/dtls.cpp
)

target_include_directories(soluna_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(OS_WINDOWS)
    target_compile_options(soluna_core PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
        $<$<CONFIG:Release>:$<IF:$<CXX_COMPILER_ID:MSVC>,/O2,-O2>>
    )
else()
    target_compile_options(soluna_core PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<CONFIG:Release>:-O2>
    )
endif()

if(OS_LINUX)
    find_library(ALSA_LIB asound)
    if(ALSA_LIB)
        target_link_libraries(soluna_core PUBLIC ${ALSA_LIB})
        target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_ALSA=1)
    endif()
elseif(OS_MACOS)
    find_library(COREAUDIO_FW CoreAudio)
    find_library(AUDIOTOOLBOX_FW AudioToolbox)
    find_library(AUDIOUNIT_FW AudioUnit)
    find_library(COREFOUNDATION_FW CoreFoundation)
    target_link_libraries(soluna_core PUBLIC
        ${COREAUDIO_FW} ${AUDIOTOOLBOX_FW} ${AUDIOUNIT_FW} ${COREFOUNDATION_FW}
    )
    target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_COREAUDIO=1)
elseif(OS_IOS)
    find_library(COREAUDIO_FW CoreAudio)
    find_library(AUDIOTOOLBOX_FW AudioToolbox)
    find_library(AUDIOUNIT_FW AudioUnit)
    find_library(COREFOUNDATION_FW CoreFoundation)
    find_library(AVFOUNDATION_FW AVFoundation)
    target_link_libraries(soluna_core PUBLIC
        ${COREAUDIO_FW} ${AUDIOTOOLBOX_FW} ${AUDIOUNIT_FW}
        ${COREFOUNDATION_FW} ${AVFOUNDATION_FW}
    )
    target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_COREAUDIO=1)
elseif(OS_ANDROID)
    target_link_libraries(soluna_core PUBLIC
        aaudio
        OpenSLES
        log
    )
    target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_AAUDIO=1)
elseif(OS_WINDOWS)
    target_link_libraries(soluna_core PUBLIC ws2_32 ole32 avrt)
    target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_WASAPI=1)
endif()

if(NOT OS_WINDOWS)
    if(NOT OS_ANDROID)
        # Android NDK has pthread built into libc, no separate library needed
        target_link_libraries(soluna_core PUBLIC pthread)
    endif()
    if(OS_LINUX OR OS_MACOS)
        # dl for dynamic loading (plugins)
        target_link_libraries(soluna_core PUBLIC dl)
    endif()
endif()

if(SOLUNA_ENABLE_AES67)
    target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_AES67=1)
endif()

if(SOLUNA_ENABLE_DTLS)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(soluna_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_DTLS=1)
endif()

if(SOLUNA_ENABLE_AAC)
    find_library(FDK_AAC_LIB fdk-aac)
    if(FDK_AAC_LIB)
        target_link_libraries(soluna_core PUBLIC ${FDK_AAC_LIB})
        target_compile_definitions(soluna_core PUBLIC SOLUNA_ENABLE_AAC=1)
    else()
        message(WARNING "FDK-AAC not found, AAC support disabled")
        set(SOLUNA_ENABLE_AAC OFF)
    endif()
endif()

if(SOLUNA_ENABLE_FLAC)
    find_library(FLAC_LIB FLAC)
    if(FLAC_LIB)
        target_link_libraries(soluna_core PUBLIC ${FLAC_LIB})
        target_compile_definitions(soluna_core PUBLIC SOLUNA_ENABLE_FLAC=1)
    else()
        message(WARNING "libFLAC not found, FLAC support disabled")
        set(SOLUNA_ENABLE_FLAC OFF)
    endif()
endif()

# Daemon
add_executable(solunad apps/daemon/main.cpp)
target_link_libraries(solunad PRIVATE soluna_core)

# CLI
add_executable(solctl apps/cli/main.cpp)
target_link_libraries(solctl PRIVATE soluna_core)

# Example plugins
add_subdirectory(examples/plugins)

# Embed web files into a C header
set(WEB_EMBED_HEADER "${CMAKE_BINARY_DIR}/generated/web_embedded.h")
add_custom_command(
    OUTPUT ${WEB_EMBED_HEADER}
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/cmake/embed_web.cmake
            ${CMAKE_SOURCE_DIR}/web ${WEB_EMBED_HEADER}
    DEPENDS ${CMAKE_SOURCE_DIR}/web/index.html
            ${CMAKE_SOURCE_DIR}/web/app.js
            ${CMAKE_SOURCE_DIR}/web/style.css
            ${CMAKE_SOURCE_DIR}/cmake/embed_web.cmake
    COMMENT "Embedding web files"
)
add_custom_target(web_embed DEPENDS ${WEB_EMBED_HEADER})
add_dependencies(soluna_core web_embed)
target_include_directories(soluna_core PUBLIC ${CMAKE_BINARY_DIR}/generated)

# Install targets
install(TARGETS solunad solctl RUNTIME DESTINATION bin)
install(DIRECTORY include/soluna DESTINATION include)

# CPack DEB support
set(CPACK_PACKAGE_NAME "soluna")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Open Network Audio System")
set(CPACK_PACKAGE_CONTACT "soluna@example.com")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Soluna Team")
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libasound2 (>= 1.1)")
include(CPack)

# Tests
if(SOLUNA_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Benchmarks
if(SOLUNA_BUILD_BENCHMARKS)
    add_subdirectory(tests/benchmark)
endif()
