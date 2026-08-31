idf_component_register(
    SRCS "app_main.c" "cws_control_protocol.c" "cws_legacy_commands.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_tsens esp_event esp_netif esp_timer esp_wifi lwip nvs_flash protocol_examples_common
)
