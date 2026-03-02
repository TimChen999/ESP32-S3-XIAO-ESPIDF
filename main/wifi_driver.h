#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Wi-Fi hardware and connect to the access point.
 * This function configures NVS, initializes the Wi-Fi driver, sets up
 * the station interface, and initiates connection.
 */
void wifi_driver_init(void);

#ifdef __cplusplus
}
#endif
