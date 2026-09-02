idf_component_register(
    SRCS "app_main.c" "cws_control_protocol.c" "cws_legacy_commands.c" "cws_probe_timing.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_tsens esp_driver_uart esp_driver_usb_serial_jtag esp_event esp_netif esp_timer esp_wifi lwip nvs_flash protocol_examples_common
)
