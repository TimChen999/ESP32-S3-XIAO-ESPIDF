idf_component_register(SRCS "main.c" "sim_modem.c" "modem_driver.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_netif esp_event driver)
