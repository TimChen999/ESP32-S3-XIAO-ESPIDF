/*
 * ============================================================================
 * NETWORK APPLICATION LAYER
 * ============================================================================
 * Prerequisites:
 * This module requires the underlying network driver (e.g., modem_driver.c or 
 * standard Wi-Fi) to be fully initialized, attached to the ESP-IDF `esp_netif` 
 * network stack, and set as the default network interface.
 * 
 * Why it is Hardware Agnostic (Cellular vs Wi-Fi):
 * The ESP-IDF TCP/IP stack (`lwIP`) completely abstracts the physical link layer.
 * Because the modem driver registers a PPP network interface and sets it as the 
 * default, this application code never interacts with the UART or AT commands. 
 * It simply waits for an "IP acquired" event from the OS, and subsequently routes 
 * all higher-level protocols (SNTP, HTTP, MQTT) seamlessly through the `esp_netif` 
 * layer, completely unaware of the underlying transmission medium.
 * ============================================================================
 */

#include "network_app.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_http_client.h"

static const char *TAG = "NET_APP";

/* FreeRTOS event group to signal when we have an IP and when time is synced */
static EventGroupHandle_t s_network_event_group;
#define IP_READY_BIT BIT0
#define TIME_SYNC_BIT BIT1

/* IP event handler. Works for BOTH Wi-Fi and PPP. 
 * Step 1: When the modem finishes dialing and PPP negotiates an IP, the 
 * underlying lwIP stack fires the IP_EVENT_PPP_GOT_IP event. 
 * We catch that event here and signal the rest of the application that 
 * the internet is now accessible.
 */
static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_PPP_GOT_IP) {
            ESP_LOGI(TAG, "Network connected via PPP modem! IP acquired.");
            xEventGroupSetBits(s_network_event_group, IP_READY_BIT);
        } else if (event_id == IP_EVENT_STA_GOT_IP) {
            ESP_LOGI(TAG, "Network connected via Wi-Fi! IP acquired.");
            xEventGroupSetBits(s_network_event_group, IP_READY_BIT);
        } else if (event_id == IP_EVENT_PPP_LOST_IP || event_id == IP_EVENT_STA_LOST_IP) {
            ESP_LOGW(TAG, "Network disconnected.");
            xEventGroupClearBits(s_network_event_group, IP_READY_BIT);
        }
    }
}

/* Callback fired when SNTP receives the time from the server 
 * Step 2: The ESP-IDF SNTP client runs in the background. Once the IP is acquired,
 * it automatically resolves the NTP server (e.g. pool.ntp.org) using DNS over PPP, 
 * sends a UDP packet, and receives the current UTC time. When successful, this 
 * callback is fired.
 */
static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronization complete");
    xEventGroupSetBits(s_network_event_group, TIME_SYNC_BIT);
}

void network_app_init(void)
{
    s_network_event_group = xEventGroupCreate();

    /* Step 0a: Register event handler for any IP event (covers both Wi-Fi and PPP)
     * This ensures our on_ip_event() is called when the network layer fully connects. */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &on_ip_event,
                                                        NULL,
                                                        NULL));

    /* Step 0b: Configure SNTP (starts automatically when IP is available)
     * Even though we don't have an IP address yet, we can initialize SNTP here.
     * The underlying stack will wait until the default network interface is UP
     * before it tries to send its UDP packets to fetch the time. */
    ESP_LOGI(TAG, "Initializing SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
    
    // Set timezone to UTC. (Example for EST: setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0", 1); tzset();)
    setenv("TZ", "UTC", 1);
    tzset();
}

void network_app_wait_for_connection(void)
{
    ESP_LOGI(TAG, "Waiting for network IP...");
    // Wait for the IP_READY_BIT to be set
    xEventGroupWaitBits(s_network_event_group, IP_READY_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "Network is ready.");
}

void network_app_wait_for_time_sync(void)
{
    ESP_LOGI(TAG, "Waiting for time synchronization (SNTP)...");
    /* Step 2b: Wait for the TIME_SYNC_BIT to be set by time_sync_notification_cb().
     * This blocks our application task until SNTP has successfully fetched the time. */
    xEventGroupWaitBits(s_network_event_group, TIME_SYNC_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Current time: %s", strftime_buf);
}

/* HTTP Event Handler (to handle data streams from HTTP responses) */
static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            // Print out the data received
            if (!esp_http_client_is_chunked_response(evt->client)) {
                printf("%.*s", evt->data_len, (char*)evt->data);
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

void network_app_test_http_request(void)
{
    /* Step 3a: Ensure we actually have an IP before attempting HTTP.
     * This blocks the task until on_ip_event() sets the IP_READY_BIT. */
    network_app_wait_for_connection();
    
    ESP_LOGI(TAG, "Starting HTTP GET request to httpbin.org...");
    
    /* Step 3b: Configure the HTTP Client.
     * When esp_http_client_perform is called, lwIP will:
     * 1. Perform a DNS lookup for "httpbin.org" over the PPP link.
     * 2. Open a TCP socket to the resolved IP address on port 80.
     * 3. Send the HTTP GET headers.
     * 4. Route all this traffic through the default netif (our modem). */
    esp_http_client_config_t config = {
        .url = "http://httpbin.org/get",
        .event_handler = _http_event_handler,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    /* Step 3c: Execute the request.
     * This triggers the _http_event_handler as data is received. */
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 (int)esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
}

// ============================================================================
//  network_app_task — The main application logic (Time sync, HTTP)
// ============================================================================
void network_app_task(void *param)
{
    ESP_LOGI(TAG, "Application task started.");

    // 1. Wait for the network link to be established (either WiFi or PPP)
    network_app_wait_for_connection();

    // 2. Wait for SNTP to sync the time
    network_app_wait_for_time_sync();

    // 3. Test direct internet connectivity (HTTP GET)
    network_app_test_http_request();

    ESP_LOGI(TAG, "Application logic complete. Suspending task.");
    vTaskDelete(NULL);
}
