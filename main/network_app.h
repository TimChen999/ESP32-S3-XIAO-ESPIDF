#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize network application layer.
 * Registers event handlers to listen for IP acquisition (PPP or Wi-Fi),
 * and configures SNTP for time synchronization.
 */
void network_app_init(void);

/**
 * @brief Wait until an IP address is acquired from any interface.
 */
void network_app_wait_for_connection(void);

/**
 * @brief Wait until time is synchronized via SNTP.
 */
void network_app_wait_for_time_sync(void);

/**
 * @brief Perform a test HTTP GET request to verify internet access.
 */
void network_app_test_http_request(void);

/**
 * @brief Main application task that runs network protocol testing.
 * Should be launched as a FreeRTOS task.
 */
void network_app_task(void *param);

#ifdef __cplusplus
}
#endif
