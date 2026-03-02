#include "wifi_driver.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

// ============================================================================
//  WIFI CONFIGURATION
// ============================================================================

#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASS      "YOUR_WIFI_PASSWORD"
#define MAXIMUM_RETRY  5

static const char *TAG = "WIFI_DRIVER";

static int s_retry_num = 0;

// ============================================================================
//  WIFI EVENT HANDLER
// ============================================================================

// ---------------------------------------------------------------------------
//  wifi_event_handler
//
//  Handles Wi-Fi state machine events (e.g., station start, disconnect) and
//  IP events (which are also handled by network_app.c, but here we track
//  connection retries).
//
//    1. On WIFI_EVENT_STA_START: Initiate the connection to the AP.
//    2. On WIFI_EVENT_STA_DISCONNECTED: Try to reconnect up to MAXIMUM_RETRY
//       times.
//    3. On IP_EVENT_STA_GOT_IP: Reset the retry counter.
// ---------------------------------------------------------------------------
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Step 1: Wi-Fi driver started, initiate connection
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // Step 2: Disconnected from AP, attempt reconnect
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect to the AP (%d/%d)", s_retry_num, MAXIMUM_RETRY);
        } else {
            ESP_LOGI(TAG, "Failed to connect to the AP");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // Step 3: IP address acquired, reset retry counter
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
    }
}

// ============================================================================
//  PUBLIC API
// ============================================================================

void wifi_driver_init(void)
{
    // -----------------------------------------------------------------------
    // Step 1: Initialize NVS (Non-Volatile Storage)
    // Wi-Fi driver needs NVS to store calibration data and saved credentials.
    // If NVS partition is corrupted or has a new format, erase it and retry.
    // -----------------------------------------------------------------------
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    // -----------------------------------------------------------------------
    // Step 2: Create the default Wi-Fi Station network interface
    // This attaches the Wi-Fi driver to the ESP-IDF TCP/IP stack (lwIP).
    // -----------------------------------------------------------------------
    esp_netif_create_default_wifi_sta();

    // -----------------------------------------------------------------------
    // Step 3: Initialize the Wi-Fi driver with default configurations
    // This allocates resources for the Wi-Fi driver and starts the Wi-Fi task.
    // -----------------------------------------------------------------------
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // -----------------------------------------------------------------------
    // Step 4: Register event handlers for Wi-Fi and IP events
    // This allows us to respond to connection/disconnection and IP acquisition.
    // -----------------------------------------------------------------------
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    // -----------------------------------------------------------------------
    // Step 5: Configure Wi-Fi connection parameters
    // Set the SSID, password, and security authentication mode.
    // -----------------------------------------------------------------------
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However, these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    // -----------------------------------------------------------------------
    // Step 6: Start the Wi-Fi driver in Station mode
    // This triggers WIFI_EVENT_STA_START, which our event handler catches
    // to call esp_wifi_connect().
    // -----------------------------------------------------------------------
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_driver_init finished.");
}
