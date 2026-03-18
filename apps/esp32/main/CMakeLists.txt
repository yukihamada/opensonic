idf_component_register(
    SRCS
        "main.c"
        "sol_i2s.c"
        "sol_net.c"
        "sol_clock.c"
        "sol_rtp.c"
        "sol_fec.c"
        "sol_mdns.c"
        "sol_nvs.c"
        "sol_engine.c"
        "sol_adpcm.c"
        "sol_security.c"
        "sol_vad_sleep.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_wifi
        esp_event
        esp_timer
        esp_netif
        lwip
        nvs_flash
)

target_compile_options(__idf_main PRIVATE -Wno-format -Wno-error)
