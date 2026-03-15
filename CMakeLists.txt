cmake_minimum_required(VERSION 3.16)
project(soluna
    VERSION 0.3.0
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

# minimp3 — fetch before core library so SOURCE_DIR is available
option(SOLUNA_HAS_MINIMP3 "Enable MP3 file playback via minimp3" ON)
if(SOLUNA_HAS_MINIMP3)
    include(FetchContent)
    FetchContent_Declare(
        minimp3
        GIT_REPOSITORY https://github.com/lieff/minimp3.git
        GIT_TAG        master
    )
    FetchContent_GetProperties(minimp3)
    if(NOT minimp3_POPULATED)
        FetchContent_Populate(minimp3)
    endif()
endif()

# Core library
add_library(soluna_core STATIC
    # PAL - Audio
    $<$<BOOL:${OS_LINUX}>:src/pal/audio/alsa.cpp>
    $<$<BOOL:${OS_MACOS}>:src/pal/audio/coreaudio.mm>
    $<$<BOOL:${OS_IOS}>:src/pal/audio/coreaudio.mm>
    $<$<BOOL:${OS_ANDROID}>:src/pal/audio/aaudio.cpp>
    $<$<BOOL:${OS_WINDOWS}>:src/pal/audio/wasapi.cpp>
    $<$<BOOL:${SOLUNA_ENABLE_PIPEWIRE}>:src/pal/audio/pipewire.cpp>
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
    src/pipeline/file_source.cpp
    # Built-in DSP plugins
    src/plugins/compressor_plugin.cpp
    src/plugins/eq_plugin.cpp
    src/plugins/reverb_plugin.cpp
    # Control
    src/control/discovery.cpp
    src/control/session_manager.cpp
    src/control/routing_matrix.cpp
    src/control/control_protocol.cpp
    src/control/rest_api.cpp
    src/control/websocket_server.cpp
    src/control/auto_router.cpp
    src/control/preset_manager.cpp
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
    src/security/license.cpp
    # Audit
    src/audit/audit_log.cpp
    # Util
    src/util/wav_writer.cpp
    # AES67 (conditional)
    $<$<BOOL:${SOLUNA_ENABLE_AES67}>:src/transport/aes67.cpp>
    # Ravenna (conditional, requires AES67)
    $<$<BOOL:${SOLUNA_ENABLE_RAVENNA}>:src/transport/ravenna.cpp>
    # DLNA/UPnP Media Renderer (conditional)
    $<$<BOOL:${SOLUNA_ENABLE_DLNA}>:src/transport/dlna_renderer.cpp>
    # AirPlay 2 receiver + sender (conditional)
    $<$<BOOL:${SOLUNA_ENABLE_AIRPLAY}>:src/transport/airplay_receiver.cpp>
    $<$<BOOL:${SOLUNA_ENABLE_AIRPLAY}>:src/transport/airplay_alac.cpp>
    $<$<BOOL:${SOLUNA_ENABLE_AIRPLAY}>:src/transport/airplay_sender.cpp>
    # DTLS (always compiled — has internal #ifdef guards for stub)
    src/transport/dtls.cpp
    # Transport Manager
    src/transport/transport_manager.cpp
)

target_include_directories(soluna_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# minimp3 include path + define (after add_library so target exists)
if(SOLUNA_HAS_MINIMP3 AND minimp3_SOURCE_DIR)
    target_include_directories(soluna_core PUBLIC ${minimp3_SOURCE_DIR})
    target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_MINIMP3)
endif()

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
    if(SOLUNA_ENABLE_PIPEWIRE)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
        target_include_directories(soluna_core PUBLIC ${PIPEWIRE_INCLUDE_DIRS})
        target_link_libraries(soluna_core PUBLIC ${PIPEWIRE_LIBRARIES})
        target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_PIPEWIRE=1)
    endif()
elseif(OS_MACOS)
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

if(SOLUNA_ENABLE_RAVENNA)
    if(NOT SOLUNA_ENABLE_AES67)
        message(FATAL_ERROR "SOLUNA_ENABLE_RAVENNA requires SOLUNA_ENABLE_AES67")
    endif()
    target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_RAVENNA=1)
endif()

if(SOLUNA_ENABLE_DLNA)
    target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_DLNA=1)
endif()

if(SOLUNA_ENABLE_AIRPLAY)
    target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_AIRPLAY=1)
    if(OS_LINUX)
        # On Linux, try to find Avahi for mDNS advertisement
        find_library(AVAHI_CLIENT_LIB avahi-client)
        find_library(AVAHI_COMMON_LIB avahi-common)
        if(AVAHI_CLIENT_LIB AND AVAHI_COMMON_LIB)
            target_link_libraries(soluna_core PUBLIC ${AVAHI_CLIENT_LIB} ${AVAHI_COMMON_LIB})
            target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_AVAHI=1)
        else()
            message(WARNING "libavahi-client not found, AirPlay mDNS on Linux will be disabled")
        endif()
    endif()
    # On macOS, dns_sd.h is available system-wide (no extra linking needed)
endif()

if(SOLUNA_ENABLE_DTLS OR SOLUNA_ENABLE_TLS)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(soluna_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    if(SOLUNA_ENABLE_DTLS)
        target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_DTLS=1)
    endif()
    if(SOLUNA_ENABLE_TLS)
        target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_TLS=1)
    endif()
endif()

if(SOLUNA_ENABLE_OPUS)
    find_library(OPUS_LIB opus)
    find_path(OPUS_INCLUDE_DIR opus/opus.h)
    if(OPUS_LIB AND OPUS_INCLUDE_DIR)
        target_link_libraries(soluna_core PUBLIC ${OPUS_LIB})
        target_include_directories(soluna_core PUBLIC ${OPUS_INCLUDE_DIR})
        target_compile_definitions(soluna_core PUBLIC SOLUNA_HAS_OPUS=1)
    else()
        message(WARNING "libopus not found, Opus support disabled")
        set(SOLUNA_ENABLE_OPUS OFF)
    endif()
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

# Daemon (not for iOS/Android)
if(NOT OS_IOS AND NOT OS_ANDROID)
    add_executable(solunad apps/daemon/main.cpp)
    target_link_libraries(solunad PRIVATE soluna_core)
endif()

# Linux/RPi unified CLI (RX + DJ mode, no soluna_core dependency)
if(OS_LINUX)
    add_executable(soluna apps/linux-rx/main.cpp)
    target_link_libraries(soluna PRIVATE asound pthread)
    target_compile_options(soluna PRIVATE -Wall -Wextra $<$<CONFIG:Release>:-O2>)
    if(SOLUNA_ENABLE_PIPEWIRE)
        target_include_directories(soluna PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
        target_link_libraries(soluna PRIVATE ${PIPEWIRE_LIBRARIES})
        target_compile_definitions(soluna PRIVATE SOLUNA_HAS_PIPEWIRE=1)
    endif()
    if(SOLUNA_ENABLE_DLNA)
        target_sources(soluna PRIVATE src/transport/dlna_renderer.cpp)
        target_include_directories(soluna PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_definitions(soluna PRIVATE SOLUNA_HAS_DLNA=1)
        if(SOLUNA_HAS_MINIMP3 AND minimp3_SOURCE_DIR)
            target_include_directories(soluna PRIVATE ${minimp3_SOURCE_DIR})
            target_compile_definitions(soluna PRIVATE SOLUNA_HAS_MINIMP3 SOLUNA_DLNA_MINIMP3_IMPL)
        endif()
    endif()
    if(SOLUNA_ENABLE_AIRPLAY)
        target_sources(soluna PRIVATE
            src/transport/airplay_receiver.cpp
            src/transport/airplay_alac.cpp
            src/transport/airplay_sender.cpp)
        target_include_directories(soluna PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_definitions(soluna PRIVATE SOLUNA_HAS_AIRPLAY=1)
        if(AVAHI_CLIENT_LIB AND AVAHI_COMMON_LIB)
            target_link_libraries(soluna PRIVATE ${AVAHI_CLIENT_LIB} ${AVAHI_COMMON_LIB})
            target_compile_definitions(soluna PRIVATE SOLUNA_HAS_AVAHI=1)
        endif()
    endif()
    install(TARGETS soluna RUNTIME DESTINATION bin)
endif()

# Windows all-in-one client (RX + TX, WASAPI loopback, no soluna_core dependency)
if(OS_WINDOWS)
    add_executable(soluna-win WIN32 apps/win/main.cpp)
    target_link_libraries(soluna-win PRIVATE ws2_32 ole32 avrt dwmapi comctl32)
    target_compile_options(soluna-win PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
        $<$<CONFIG:Release>:$<IF:$<CXX_COMPILER_ID:MSVC>,/O2,-O2>>
    )
    target_compile_definitions(soluna-win PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX
    )
    install(TARGETS soluna-win RUNTIME DESTINATION bin)

    # Legacy standalone RX (kept for compatibility)
    add_executable(soluna-rx-win apps/win-rx/main.cpp)
    target_link_libraries(soluna-rx-win PRIVATE ws2_32 ole32 avrt)
    target_compile_definitions(soluna-rx-win PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(soluna-rx-win PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
        $<$<CONFIG:Release>:$<IF:$<CXX_COMPILER_ID:MSVC>,/O2,-O2>>
    )
endif()

# WAN relay server (standalone, no soluna_core dependency)
if(NOT OS_IOS AND NOT OS_ANDROID)
    add_executable(soluna-relay apps/relay/main.cpp)
    if(NOT OS_WINDOWS)
        target_link_libraries(soluna-relay PRIVATE pthread)
    endif()
    target_compile_options(soluna-relay PRIVATE -Wall -Wextra $<$<CONFIG:Release>:-O2>)

    # Relay requires OpenSSL, libcurl, and SQLite3
    find_package(OpenSSL REQUIRED)
    target_link_libraries(soluna-relay PRIVATE OpenSSL::SSL OpenSSL::Crypto)

    find_package(CURL REQUIRED)
    target_link_libraries(soluna-relay PRIVATE CURL::libcurl)

    find_package(SQLite3)
    if(SQLite3_FOUND)
        target_link_libraries(soluna-relay PRIVATE SQLite::SQLite3)
    else()
        find_library(SQLITE3_LIB sqlite3)
        find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
        if(SQLITE3_LIB AND SQLITE3_INCLUDE_DIR)
            target_link_libraries(soluna-relay PRIVATE ${SQLITE3_LIB})
            target_include_directories(soluna-relay PRIVATE ${SQLITE3_INCLUDE_DIR})
        else()
            message(FATAL_ERROR "SQLite3 not found, required by soluna-relay")
        endif()
    endif()

    install(TARGETS soluna-relay RUNTIME DESTINATION bin)
endif()


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

# Install targets (not for iOS/Android)
if(NOT OS_IOS AND NOT OS_ANDROID)
    install(TARGETS solunad RUNTIME DESTINATION bin)
endif()
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
