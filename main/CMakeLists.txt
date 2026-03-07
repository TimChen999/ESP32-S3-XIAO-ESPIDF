idf_component_register(SRCS "network_app.c" "main.c" "sim_modem.c" "modem_driver.c" "wifi_driver.c" "test_network.c" "speaker_driver.c" "mic_driver.c" "voice_assistant.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_netif esp_event driver esp_http_client esp_wifi nvs_flash)
