#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "sim_modem.h"
#include "modem_driver.h"
#include "network_app.h"
#include "wifi_driver.h"
#include "test_network.h"
#include "speaker_driver.h"

#define TASK_STACK_SIZE     4096
#define MODEM_SIM_CORE      0
#define DRIVER_CORE         1
#define TASK_PRIORITY       5

static const char *TAG = "MAIN";

void app_main(void)
{
    printf("\n========================================\n");
    printf("  Cellular Modem Simulator — ESP32-S3\n");
    printf("========================================\n\n");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    network_app_init();

    // OPTION A: Wi-Fi
    wifi_driver_init();

    // OPTION B: Cellular Modem (uncomment below, comment out Wi-Fi above)
    // sim_modem_init();
    // modem_driver_init();
    // xTaskCreatePinnedToCore(sim_modem_task, "sim_modem",
    //     TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL, MODEM_SIM_CORE);
    // xTaskCreatePinnedToCore(modem_driver_task, "modem_driver",
    //     TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL, DRIVER_CORE);

    // Audio output — I2S speaker driver
    speaker_init();
    xTaskCreatePinnedToCore(speaker_playback_task, "speaker",
        TASK_STACK_SIZE, NULL, TASK_PRIORITY + 1, NULL, DRIVER_CORE);

    xTaskCreatePinnedToCore(network_app_task, "app_task",
        TASK_STACK_SIZE * 2, NULL, TASK_PRIORITY - 1, NULL, DRIVER_CORE);

    xTaskCreatePinnedToCore(test_network_task, "test_task",
        TASK_STACK_SIZE * 2, NULL, TASK_PRIORITY - 1, NULL, DRIVER_CORE);

    ESP_LOGI(TAG, "Tasks launched. Returning to FreeRTOS scheduler.");
}
