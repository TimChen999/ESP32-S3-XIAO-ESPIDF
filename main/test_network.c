/*
 * ============================================================================
 * NETWORK TEST MODULE
 * ============================================================================
 * This module tests simple internet communication completely agnostic 
 * of the underlying network interface (Cellular vs. Wi-Fi).
 *
 * RELATION TO sim_modem:
 * As described in `sim_modem.h`, the modem handles Layer 1 (UART electrical 
 * signals) and Layer 2 (PPP/HDLC framing). The ESP-IDF TCP/IP stack (`lwIP`)
 * abstracts all of this. 
 *
 * By the time this test code runs, `sim_modem` has already negotiated an IP 
 * address via PPP, or a Wi-Fi driver has gotten an IP. This application code 
 * sits at Layer 7 and simply uses sockets and `esp_http_client` without 
 * needing to know if the packets are being sent over Wi-Fi airwaves or 
 * bit-banged over a UART PPP connection to the simulated modem.
 *
 * WOKWI COMPATIBILITY:
 * This code runs unmodified in the Wokwi simulator. Wokwi's virtual gateway 
 * transparently proxies the standard ESP-IDF lwIP socket requests out to the 
 * real internet, typically using the simulated Wokwi-GUEST Wi-Fi network.
 * ============================================================================
 */

#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "lwip/err.h"
#include "lwip/sockets.h"

#define GOOGLE_SEARCH_QUERY "Clavicular framemogged by ASU frat leader??"

static const char *TAG = "TEST_NET";

/* --------------------------------------------------------------------------
 *  HTTP Test
 *  Sends a simple GET request using esp_http_client
 * -------------------------------------------------------------------------- */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                ESP_LOGI(TAG, "Data chunk received: %.*s", evt->data_len, (char*)evt->data);
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

void test_network_http_get(void)
{
    ESP_LOGI(TAG, "Starting agnostic HTTP GET test...");

    // We assume the network is already connected and an IP is acquired 
    // before calling this function (either via sim_modem/modem_driver or wifi).
    
    // We use a simple HTTP endpoint. Wokwi supports standard HTTP out to the internet.
    esp_http_client_config_t config = {
        .url = "http://httpbin.org/get",
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP GET Success! Status = %d, Content-Length = %d",
                 esp_http_client_get_status_code(client),
                 (int)esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
}

/* --------------------------------------------------------------------------
 *  Google Search Test
 *  Searches google with a predetermined query and returns info about the results
 * -------------------------------------------------------------------------- */
void test_network_google_search(void)
{
    ESP_LOGI(TAG, "Starting Google Search test...");

    // Dynamically build the URL from the predetermined query
    char url[256];
    snprintf(url, sizeof(url), "http://www.google.com/search?q=%s", GOOGLE_SEARCH_QUERY);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
        // Since Google might redirect HTTP to HTTPS, we might encounter a 301/302.
        // For simplicity, we just print whatever we get (either the redirect or the HTML).
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return;
    }

    ESP_LOGI(TAG, "Sending GET request to %s", url);
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);
        
        ESP_LOGI(TAG, "Google Search Request Success!");
        ESP_LOGI(TAG, "Status Code = %d", status_code);
        ESP_LOGI(TAG, "Content-Length = %d bytes", content_length);
        
        if (status_code == 200) {
            ESP_LOGI(TAG, "Successfully retrieved search results HTML.");
        } else if (status_code == 301 || status_code == 302) {
            ESP_LOGW(TAG, "Google returned a redirect (likely to HTTPS).");
        }
    } else {
        ESP_LOGE(TAG, "Google Search request failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
}

/* --------------------------------------------------------------------------
 *  Test Task Wrapper
 * -------------------------------------------------------------------------- */
void test_network_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Test network task started.");

    // Wait for the IP to be acquired and time to sync
    // Because we export the wait functions in network_app.h, we can use them!
    extern void network_app_wait_for_connection(void);
    extern void network_app_wait_for_time_sync(void);
    
    network_app_wait_for_connection();
    network_app_wait_for_time_sync();
    
    // Run HTTP test
    test_network_http_get();

    vTaskDelay(pdMS_TO_TICKS(2000));

    // Run Google Search test
    test_network_google_search();

    ESP_LOGI(TAG, "Test network task finished. Deleting task.");
    vTaskDelete(NULL);
}
