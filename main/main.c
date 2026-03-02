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

// ============================================================================
//  PIN & UART CONFIGURATION
//
//  All pin assignments in one place. Change these if you rewire the diagram.
//
//  Physical wiring (see diagram.json):
//    Driver TX  (D0) ───green wire──► Modem RX  (D8)
//    Modem TX   (D5) ───blue wire───► Driver RX (D1)
//
//  Flow control wiring (optional — enable FLOW_CONTROL_ENABLED below):
//    Driver RTS (D2) ───yellow wire─► Modem CTS  (D9)
//    Modem RTS  (D4) ───orange wire─► Driver CTS (D3)
//
//  UART0 (D6=TX, D7=RX) is reserved for the console — printf() goes there.
// ============================================================================

// --- Flow control toggle ---
// Set to 1 to enable RTS/CTS hardware flow control.
// Start with 0 — enable after basic AT commands work correctly.
// See DESIGN.md "Hardware Flow Control" section for guidance.
#define FLOW_CONTROL_ENABLED  0

// --- Driver side (UART1) — talks TO the modem ---
#define DRIVER_UART_NUM     1
#define DRIVER_TX_PIN       1            // D0 on XIAO (GPIO1)
#define DRIVER_RX_PIN       2            // D1 on XIAO (GPIO2)
#define DRIVER_RTS_PIN      3            // D2 on XIAO (GPIO3, flow control)
#define DRIVER_CTS_PIN      4            // D3 on XIAO (GPIO4, flow control)

// --- Simulated modem side (UART2) — IS the modem ---
#define MODEM_SIM_UART_NUM  2
#define MODEM_SIM_TX_PIN    6            // D5 on XIAO (GPIO6)
#define MODEM_SIM_RX_PIN    7            // D8 on XIAO (GPIO7)
#define MODEM_SIM_RTS_PIN   5            // D4 on XIAO (GPIO5, flow control)
#define MODEM_SIM_CTS_PIN   8            // D9 on XIAO (GPIO8, flow control)

// --- Shared UART settings ---
#define UART_BAUD           115200
#define FLOW_CTRL_THRESH    122          // RX FIFO fill level to assert RTS

// --- FreeRTOS task settings ---
#define TASK_STACK_SIZE     4096
#define MODEM_SIM_CORE      0            // Sim modem runs on core 0
#define DRIVER_CORE         1            // Driver/app runs on core 1
#define MODEM_SIM_PRIORITY  5
#define DRIVER_PRIORITY     5

static const char *TAG = "MAIN";

// ============================================================================
//  Configs — passed by pointer to each task
// ============================================================================

static sim_modem_config_t sim_modem_cfg = {
    .uart_num      = MODEM_SIM_UART_NUM,
    .tx_pin        = MODEM_SIM_TX_PIN,
    .rx_pin        = MODEM_SIM_RX_PIN,
    .rts_pin       = FLOW_CONTROL_ENABLED ? MODEM_SIM_RTS_PIN : -1, // -1 = no RTS pin assigned
    .cts_pin       = FLOW_CONTROL_ENABLED ? MODEM_SIM_CTS_PIN : -1, // -1 = no CTS pin assigned
    .baud_rate     = UART_BAUD,
    .flow_control  = FLOW_CONTROL_ENABLED,
    .state         = SIM_MODEM_STATE_OFF,
};

static modem_driver_config_t modem_driver_cfg = {
    .uart_num      = DRIVER_UART_NUM,
    .tx_pin        = DRIVER_TX_PIN,
    .rx_pin        = DRIVER_RX_PIN,
    .rts_pin       = FLOW_CONTROL_ENABLED ? DRIVER_RTS_PIN : -1, // -1 = no RTS pin assigned
    .cts_pin       = FLOW_CONTROL_ENABLED ? DRIVER_CTS_PIN : -1, // -1 = no CTS pin assigned
    .baud_rate     = UART_BAUD,
    .flow_control  = FLOW_CONTROL_ENABLED,
    .state         = MODEM_DRIVER_IDLE,
};

// ============================================================================
//  app_main — entry point
//
//  Flow:
//    1. Print banner to console (UART0)
//    2. Initialize both UARTs
//    3. Launch sim_modem_task on core 0  (the "hardware" modem)
//    4. Launch modem_driver_task on core 1 (the application/driver)
//    5. Return — FreeRTOS scheduler takes over
// ============================================================================

void app_main(void)
{
    printf("\n========================================\n");
    printf("  Cellular Modem Simulator — ESP32-S3\n");
    printf("========================================\n");
    printf("Driver  UART%d: TX=GPIO%d(D0) RX=GPIO%d(D1)\n",
           DRIVER_UART_NUM, DRIVER_TX_PIN, DRIVER_RX_PIN);
    printf("Sim Modem UART%d: TX=GPIO%d(D5) RX=GPIO%d(D8)\n",
           MODEM_SIM_UART_NUM, MODEM_SIM_TX_PIN, MODEM_SIM_RX_PIN);
    printf("Baud: %d\n", UART_BAUD);
    if (FLOW_CONTROL_ENABLED) {
        printf("Flow control: RTS/CTS ENABLED\n");
        printf("  Driver  RTS=GPIO%d(D2) CTS=GPIO%d(D3)\n",
               DRIVER_RTS_PIN, DRIVER_CTS_PIN);
        printf("  Modem   RTS=GPIO%d(D4) CTS=GPIO%d(D9)\n",
               MODEM_SIM_RTS_PIN, MODEM_SIM_CTS_PIN);
    } else {
        printf("Flow control: DISABLED\n");
    }
    printf("========================================\n\n");

    // --- Initialize TCP/IP stack and event loop (required for PPP Phase 2) ---
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // --- Initialize network application layer (events, SNTP, etc.) ---
    network_app_init();

    // -----------------------------------------------------
    // OPTION A: Test using Wi-Fi Driver
    // -----------------------------------------------------
    wifi_driver_init();

    // -----------------------------------------------------
    // OPTION B: Test using Cellular Modem Driver
    // -----------------------------------------------------
    // sim_modem_init(&sim_modem_cfg);
    // modem_driver_init(&modem_driver_cfg);

    // --- Launch the simulated modem FIRST (it needs to be listening) ---
    // xTaskCreatePinnedToCore(
    //     sim_modem_task,          // Task function
    //     "sim_modem",             // Name (for debugging)
    //     TASK_STACK_SIZE,         // Stack size in bytes
    //     &sim_modem_cfg,          // Parameter passed to task
    //     MODEM_SIM_PRIORITY,      // Priority
    //     NULL,                    // Task handle (not needed)
    //     MODEM_SIM_CORE           // Core to pin to
    // );

    // --- Then launch the driver/application ---
    // xTaskCreatePinnedToCore(
    //     modem_driver_task,       // Task function
    //     "modem_driver",          // Name
    //     TASK_STACK_SIZE,         // Stack size
    //     &modem_driver_cfg,       // Parameter passed to task
    //     DRIVER_PRIORITY,         // Priority
    //     NULL,                    // Task handle
    //     DRIVER_CORE              // Core to pin to
    // );

    // --- Launch the main application task (wait for network, sync time, HTTP) ---
    xTaskCreatePinnedToCore(
        network_app_task,
        "app_task",
        TASK_STACK_SIZE * 2,     // Need larger stack for HTTP
        NULL,
        DRIVER_PRIORITY - 1,     // Slightly lower priority than network driver
        NULL,
        DRIVER_CORE
    );

    ESP_LOGI(TAG, "Tasks launched. Returning to FreeRTOS scheduler.");
}
