include(GoogleTest)

# Phase 1 Unit tests
add_executable(test_ring_buffer unit/test_ring_buffer.cpp)
target_link_libraries(test_ring_buffer PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_ring_buffer)

add_executable(test_ostp unit/test_ostp.cpp)
target_link_libraries(test_ostp PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_ostp)

add_executable(test_format_converter unit/test_format_converter.cpp)
target_link_libraries(test_format_converter PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_format_converter)

add_executable(test_clock unit/test_clock.cpp)
target_link_libraries(test_clock PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_clock)

# Phase 2 Unit tests
add_executable(test_ptp unit/test_ptp.cpp)
target_link_libraries(test_ptp PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_ptp)

add_executable(test_bmca unit/test_bmca.cpp)
target_link_libraries(test_bmca PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_bmca)

add_executable(test_clock_servo unit/test_clock_servo.cpp)
target_link_libraries(test_clock_servo PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_clock_servo)

add_executable(test_adaptive_pll unit/test_adaptive_pll.cpp)
target_link_libraries(test_adaptive_pll PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_adaptive_pll)

add_executable(test_playout_buffer unit/test_playout_buffer.cpp)
target_link_libraries(test_playout_buffer PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_playout_buffer)

# Integration tests
add_executable(test_loopback integration/test_loopback.cpp)
target_link_libraries(test_loopback PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_loopback)

add_executable(test_clock_sync integration/test_clock_sync.cpp)
target_link_libraries(test_clock_sync PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_clock_sync)

# Phase 3 Unit tests
add_executable(test_routing unit/test_routing.cpp)
target_link_libraries(test_routing PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_routing)

add_executable(test_discovery unit/test_discovery.cpp)
target_link_libraries(test_discovery PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_discovery)

add_executable(test_session unit/test_session.cpp)
target_link_libraries(test_session PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_session)

add_executable(test_protocol unit/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_protocol)

add_executable(test_control integration/test_control.cpp)
target_link_libraries(test_control PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_control)

# Phase 4 Unit tests
add_executable(test_jitter_buffer unit/test_jitter_buffer.cpp)
target_link_libraries(test_jitter_buffer PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_jitter_buffer)

add_executable(test_fec unit/test_fec.cpp)
target_link_libraries(test_fec PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_fec)

add_executable(test_adaptation unit/test_adaptation.cpp)
target_link_libraries(test_adaptation PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_adaptation)

add_executable(test_opus unit/test_opus.cpp)
target_link_libraries(test_opus PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_opus)

add_executable(test_wifi integration/test_wifi.cpp)
target_link_libraries(test_wifi PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_wifi)

# Phase 5 Unit tests
add_executable(test_pal unit/test_pal.cpp)
target_link_libraries(test_pal PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_pal)

add_executable(test_cross_platform integration/test_cross_platform.cpp)
target_link_libraries(test_cross_platform PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_cross_platform)

# Phase 6 Unit tests (ESP32 algorithms, desktop-testable)
add_executable(test_esp32_fec unit/test_esp32_fec.cpp)
target_link_libraries(test_esp32_fec PRIVATE GTest::gtest_main)
gtest_discover_tests(test_esp32_fec)

# Phase 7 Unit tests
if(SOLUNA_ENABLE_AES67)
    add_executable(test_aes67 unit/test_aes67.cpp)
    target_link_libraries(test_aes67 PRIVATE soluna_core GTest::gtest_main)
    gtest_discover_tests(test_aes67)
endif()

add_executable(test_dsp_plugin unit/test_dsp_plugin.cpp)
target_link_libraries(test_dsp_plugin PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_dsp_plugin)

add_executable(test_websocket unit/test_websocket.cpp)
target_link_libraries(test_websocket PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_websocket)

add_executable(test_dtls unit/test_dtls.cpp)
target_link_libraries(test_dtls PRIVATE soluna_core GTest::gtest_main)
if(SOLUNA_ENABLE_DTLS)
    target_link_libraries(test_dtls PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
gtest_discover_tests(test_dtls)

# Phase 8 Unit tests
add_executable(test_error unit/test_error.cpp)
target_link_libraries(test_error PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_error)

add_executable(test_config unit/test_config.cpp)
target_link_libraries(test_config PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_config)

add_executable(test_metrics unit/test_metrics.cpp)
target_link_libraries(test_metrics PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_metrics)

add_executable(test_auth unit/test_auth.cpp)
target_link_libraries(test_auth PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_auth)

add_executable(test_audit unit/test_audit.cpp)
target_link_libraries(test_audit PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_audit)

add_executable(test_codec unit/test_codec.cpp)
target_link_libraries(test_codec PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_codec)

add_executable(test_auto_router unit/test_auto_router.cpp)
target_link_libraries(test_auto_router PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_auto_router)

add_executable(test_dsp_chain unit/test_dsp_chain.cpp)
target_link_libraries(test_dsp_chain PRIVATE soluna_core GTest::gtest_main)
gtest_discover_tests(test_dsp_chain)

# v0.2.0 Integration tests
add_executable(test_dtls_transport integration/test_dtls_transport.cpp)
target_link_libraries(test_dtls_transport PRIVATE soluna_core GTest::gtest_main)
if(SOLUNA_ENABLE_DTLS)
    target_link_libraries(test_dtls_transport PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
gtest_discover_tests(test_dtls_transport)

if(SOLUNA_ENABLE_AES67)
    add_executable(test_aes67_loopback integration/test_aes67_loopback.cpp)
    target_link_libraries(test_aes67_loopback PRIVATE soluna_core GTest::gtest_main)
    gtest_discover_tests(test_aes67_loopback)
endif()

# Stress tests (opt-in)
if(SOLUNA_BUILD_STRESS_TESTS)
    add_executable(test_multichannel stress/test_multichannel.cpp)
    target_link_libraries(test_multichannel PRIVATE soluna_core GTest::gtest_main)
    gtest_discover_tests(test_multichannel)

    add_executable(test_multidevice stress/test_multidevice.cpp)
    target_link_libraries(test_multidevice PRIVATE soluna_core GTest::gtest_main)
    gtest_discover_tests(test_multidevice)

    add_executable(test_longevity stress/test_longevity.cpp)
    target_link_libraries(test_longevity PRIVATE soluna_core GTest::gtest_main)
    gtest_discover_tests(test_longevity)
endif()
