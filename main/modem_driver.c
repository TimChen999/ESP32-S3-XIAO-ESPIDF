#include "modem_driver.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_event.h"
#include "driver/uart.h"

static const char *TAG = "MODEM_DRV";

// SIM PIN: Set modem PIN TODO: This should come from some config file or smth
#define DEFAULT_SIM_PIN "0000"

// MAX FAILURES: the max number of time modem handshake can fail before fatal
#define MAX_FAILS_GETIP 10

// Echo state: Will it echo? True when ATE0 succeeds so read_response can skip stripping
static bool g_echo_disabled = false;

// Config pointer for modem_driver_get_state (set in init, used by task)
static modem_driver_config_t *s_config = NULL;

// ============================================================================
//  INTERNAL HELPERS
// ============================================================================

// ---------------------------------------------------------------------------
//  send_command_raw
//
//  Sends a single AT command to the modem over UART. The modem expects
//  commands terminated by '\r' (carriage return). Echo handling is done
//  in read_response — we scan for terminators so echoed bytes are harmless.
//
//    1. Write the command bytes using uart_write_bytes()
//    2. Write a '\r' byte to terminate the command (Hayes standard)
//    3. Log the command for debugging
// ---------------------------------------------------------------------------
static void send_command_raw(modem_driver_config_t *config, const char *cmd)
{
    uart_write_bytes(config->uart_num, cmd, strlen(cmd));
    uart_write_bytes(config->uart_num, "\r", 1);
    ESP_LOGI(TAG, "TX: %s", cmd);
}

// ---------------------------------------------------------------------------
//  read_response
//
//  Reads bytes from UART RX until a known terminator is found in the buffer.
//  Modem responses end with one of: "OK", "ERROR", "CONNECT", "NO CARRIER".
//
//    1. Read one byte at a time using uart_read_bytes() with the given timeout
//    2. After each byte, null-terminate and check if buffer contains any
//       terminator (strstr for "OK", "ERROR", "CONNECT", "NO CARRIER")
//    3. If found, return the total length so caller can parse with strstr()
//    4. If timeout (got <= 0), return -1; if buffer full without terminator,
//       return -1 (garbled — caller may flush and retry)
// ---------------------------------------------------------------------------
static int read_response(modem_driver_config_t *config,
                         char *buf, size_t buf_size,
                         TickType_t timeout)
{
    if (buf_size < 2) return -1;
    size_t len = 0;
    buf[0] = '\0';

    while (len < buf_size - 1) {
        char c;
        int got = uart_read_bytes(config->uart_num, (unsigned char *)&c, 1, timeout);
        if (got <= 0) {
            return (len > 0) ? (int)len : -1;
        }
        buf[len++] = c;
        buf[len] = '\0';

        if (strstr(buf, "OK") || strstr(buf, "ERROR") ||
            strstr(buf, "CONNECT") || strstr(buf, "NO CARRIER")) {
            // Echo not disabled, strip the echoed part out
            if (!g_echo_disabled) {
                char *first_crlf = strstr(buf, "\r\n");
                if (first_crlf != NULL && (size_t)(first_crlf - buf) < len) {
                    size_t skip = (size_t)(first_crlf - buf) + 2;
                    if (skip < len) {
                        memmove(buf, buf + skip, len - skip + 1);
                        return (int)(len - skip);
                    }
                }
            }
            return (int)len;
        }
    }
    return -1; /* buffer full without terminator (garbled) */
}

// ---------------------------------------------------------------------------
//  response_contains
//
//  Returns true if the response buffer contains the given substring. Callers
//  use this to detect "OK", "+CPIN: READY", "+CREG: 0,1", "CONNECT", etc.,
//  without assuming the response starts at index 0 (echo may prefix it).
// ---------------------------------------------------------------------------
static bool response_contains(const char *response, const char *needle)
{
    return (response != NULL) && (strstr(response, needle) != NULL);
}


// ============================================================================
//  PUBLIC API
// ============================================================================

modem_driver_state_t modem_driver_get_state(void)
{
    return (s_config != NULL) ? s_config->state : MODEM_DRIVER_IDLE;
}

void modem_driver_init(modem_driver_config_t *config)
{
    s_config = config;
    // 1. Set state variable in config to IDLE (init), used for debug/observability
    config->state = MODEM_DRIVER_IDLE;

    // 2. Describe the UART's electrical parameters: baud rate, word format,
    //    and flow control. Must match what the modem on the other end expects.
    //    8N1 (8 data bits, no parity, 1 stop bit) is universal for AT modems.
    uart_config_t uart_cfg = {
        .baud_rate           = config->baud_rate,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = config->flow_control
                                   ? UART_HW_FLOWCTRL_CTS_RTS
                                   : UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
    };

    // 3. Apply the config to the UART peripheral registers. After this,
    //    the hardware knows the baud rate and framing but has no pin
    //    assignments or software buffers yet.
    uart_param_config(config->uart_num, &uart_cfg);

    // 4. Route the UART's TX, RX, RTS, and CTS signals to physical GPIO pins
    //    via the GPIO matrix. When flow control is off, rts_pin and cts_pin
    //    are -1, so those signals stay unconnected.
    uart_set_pin(config->uart_num,
                 config->tx_pin, config->rx_pin,
                 config->rts_pin, config->cts_pin);

    // 5. Install the UART driver: allocate RX and TX ring buffers (1024-byte)
    //    so uart_read_bytes() / uart_write_bytes() can be used by
    //    send_command_raw() and read_response().
    uart_driver_install(config->uart_num, 1024, 1024, 0, NULL, 0);

    ESP_LOGI(TAG, "modem_driver_init: uart=%d tx=%d rx=%d baud=%d",
             config->uart_num, config->tx_pin, config->rx_pin,
             config->baud_rate);
}

/* Number of send+read attempts in modem_send_at on timeout/garbled. */
#define MODEM_SEND_AT_RETRIES 3

// ---------------------------------------------------------------------------
//  modem_send_at
//
//  Sends one AT command and reads the response. Used by all higher-level
//  functions (modem_check_sim, modem_register_network, etc.).
//
//    1. Flush any stale data in the UART RX buffer so we don't see old
//       responses or garbage from a previous command.
//    2. Send the command (send_command_raw) and read until a terminator
//       (read_response). On success, log the response and return length.
//    3. On timeout or garbled (buffer full without terminator), flush RX
//       and retry up to MODEM_SEND_AT_RETRIES. Return -1 if all attempts fail.
// ---------------------------------------------------------------------------
int modem_send_at(modem_driver_config_t *config,
                  const char *cmd,
                  char *resp_buf, size_t resp_buf_size,
                  TickType_t timeout)
{
    uart_flush_input(config->uart_num);

    for (int attempt = 0; attempt < MODEM_SEND_AT_RETRIES; attempt++) {
        send_command_raw(config, cmd);
        int n = read_response(config, resp_buf, resp_buf_size, timeout);
        if (n >= 0) {
            ESP_LOGI(TAG, "RX: %.*s", n, resp_buf);
            return n;
        }
        uart_flush_input(config->uart_num);
    }
    return -1;
}

// ---------------------------------------------------------------------------
//  modem_check_sim
//
//  Verifies the modem is alive and the SIM is ready. Called after the
//  task has waited for modem boot. Sends: AT (ping), ATE0 (disable echo),
//  AT+CPIN? (SIM status). On real hardware may see SIM PIN, SIM PUK, or
//  CME errors — those are handled as fatal (return -1) where appropriate.
// ---------------------------------------------------------------------------
int modem_check_sim(modem_driver_config_t *config)
{
    char resp[256];
    bool success = false;

    TickType_t at_timeout = pdMS_TO_TICKS(4000);

    // -----------------------------------------------------------------------
    // Step 1: Ping the modem with "AT".
    // Retry up to 5 times with 1s delay — modem may still be booting.
    // -----------------------------------------------------------------------
    for (int i = 0; i < 5; i++) {
        int n = modem_send_at(config, "AT", resp, sizeof(resp), at_timeout);
        if (n >= 0 && response_contains(resp, "OK")) {
            ESP_LOGI(TAG, "Initial ping OK");
            success = true;
            break;
        }
        if (n < 0) {
            ESP_LOGW(TAG, "AT timeout, retry %d/5", i + 1);
        } else if (response_contains(resp, "+CFUN")) {
            /* Unsolicited power-on notification — ignore and retry */
        } else {
            ESP_LOGW(TAG, "AT error/garbled, retry");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!success) return -1;

    at_timeout = pdMS_TO_TICKS(2000);
    bool echohandling = true;

    // -----------------------------------------------------------------------
    // Step 2: Disable echo with "ATE0".
    // If it fails after 3 tries we continue anyway — read_response handles
    // echo by scanning for terminators; we just log and proceed.
    // -----------------------------------------------------------------------
    for (int i = 0; i < 3; i++) {
        int n = modem_send_at(config, "ATE0", resp, sizeof(resp), at_timeout);
        if (n >= 0 && response_contains(resp, "OK")) {
            echohandling = false;
            g_echo_disabled = true;  /* read_response will no longer strip first line */
            ESP_LOGI(TAG, "Echo disabled");
            break;
        }
        if (n < 0) ESP_LOGW(TAG, "ATE0 timeout");
        else ESP_LOGW(TAG, "ATE0 error");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (echohandling) ESP_LOGW(TAG, "Echo disable failed, continuing with echo handling");

    // -----------------------------------------------------------------------
    // Step 3: Check SIM with "AT+CPIN?".
    // READY → success. SIM PIN → send PIN (DEFAULT_SIM_PIN), then success if OK.
    // SIM PUK / CME 10 (no SIM) / CME 13 (SIM failure) → fatal (return -1).
    // -----------------------------------------------------------------------
    for (int i = 0; i < 3; i++) {
        int n = modem_send_at(config, "AT+CPIN?", resp, sizeof(resp), at_timeout);
        if (n >= 0 && response_contains(resp, "+CPIN: READY")) {
            config->state = MODEM_DRIVER_SIM_OK;
            ESP_LOGI(TAG, "SIM ready");
            return 0;
        }
        if (n >= 0 && response_contains(resp, "+CPIN: SIM PIN")) {
            n = modem_send_at(config, "AT+CPIN=\"" DEFAULT_SIM_PIN "\"", resp, sizeof(resp), at_timeout);
            if (n >= 0 && response_contains(resp, "OK")) {
                config->state = MODEM_DRIVER_SIM_OK;
                ESP_LOGI(TAG, "SIM ready (PIN accepted)");
                return 0;
            }
            ESP_LOGE(TAG, "SIM PIN unknown or wrong");
            return -1;
        }
        if (n >= 0 && response_contains(resp, "SIM PUK")) {
            ESP_LOGE(TAG, "SIM PUK locked");
            return -1;
        }
        if (n >= 0 && response_contains(resp, "+CME ERROR: 10")) {
            ESP_LOGE(TAG, "SIM not inserted");
            return -1;
        }
        if (n >= 0 && response_contains(resp, "+CME ERROR: 13")) {
            ESP_LOGE(TAG, "SIM failure");
            return -1;
        }
        if (n < 0) ESP_LOGW(TAG, "AT+CPIN? timeout");
        else ESP_LOGW(TAG, "AT+CPIN? garbled or other error");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "SIM check failed after 3 tries");
    return -1;
}

// ---------------------------------------------------------------------------
//  modem_register_network
//
//  Registers on the cellular network. Sends AT+CSQ (signal quality), then
//  polls AT+CREG? until registered (stat 1 = home, 5 = roaming). On success
//  sends AT+COPS? (operator name), sets state REGISTERED, and returns 0.
// ---------------------------------------------------------------------------
int modem_register_network(modem_driver_config_t *config)
{
    char resp[256];
    int rssi = 0, ber = 0;

    TickType_t at_timeout = pdMS_TO_TICKS(2000);

    // Step 1: Check signal quality (informational; we log RSSI/BER).
    int n = modem_send_at(config, "AT+CSQ", resp, sizeof(resp), at_timeout);
    if (n >= 0) {
        const char *csq = strstr(resp, "+CSQ:");
        if (csq && sscanf(csq, "+CSQ: %d,%d", &rssi, &ber) >= 2) {
            ESP_LOGI(TAG, "Signal: RSSI=%d, BER=%d", rssi, ber);
        }
    }

    // Step 2: Poll registration status. CREG second value: 1 = home, 5 = roaming,
    // 2 = searching, 3 = denied. We retry up to 10 times; if 2, wait 2s and continue.
    for (int i = 0; i < 10; i++) {
        n = modem_send_at(config, "AT+CREG?", resp, sizeof(resp), at_timeout);
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        const char *creg = strstr(resp, "+CREG:");
        if (!creg) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        int n_val = -1, stat = -1;
        sscanf(creg, "+CREG: %d,%d", &n_val, &stat);
        if (stat == 1 || stat == 5) {
            ESP_LOGI(TAG, "CREG %d — registration success", stat);
            // Step 3: Query operator name (log only).
            n = modem_send_at(config, "AT+COPS?", resp, sizeof(resp), at_timeout);
            if (n >= 0) {
                const char *op = strstr(resp, "+COPS:");
                if (op) ESP_LOGI(TAG, "Operator: %s", op);
            }
            // Step 4: Set state and report success.
            config->state = MODEM_DRIVER_REGISTERED;
            printf("Registered on network\n");
            return 0;
        }
        if (stat == 2) {
            ESP_LOGI(TAG, "Waiting for registration...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (stat == 3) {
            ESP_LOGE(TAG, "Registration denied");
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGE(TAG, "Registration failed after 10 tries");
    return -1;
}

// ---------------------------------------------------------------------------
//  modem_activate_pdp
//
//  Defines the PDP context (CGDCONT) and activates it (CGACT). Define only
//  tells the modem "context 1 = IP, APN internet"; activate actually brings
//  up the data bearer. On real hardware, CGACT can take 5–30s; we use a
//  long timeout. CME 30 (no network) / 33 (not subscribed) → fatal. CME 148
//  → retry once, then fail.
// ---------------------------------------------------------------------------
int modem_activate_pdp(modem_driver_config_t *config)
{
    char resp[256];

    TickType_t at_timeout = pdMS_TO_TICKS(2000);
    TickType_t pdp_timeout = pdMS_TO_TICKS(30000);

    // Step 1: Define PDP context — context ID 1, type IP, APN "internet".
    // Define only declares the parameters; the modem does not bring up
    // the bearer until we send CGACT=1,1.
    int n = modem_send_at(config, "AT+CGDCONT=1,\"IP\",\"internet\"",
                          resp, sizeof(resp), at_timeout);
    if (n < 0 || !response_contains(resp, "OK")) {
        ESP_LOGE(TAG, "PDP define (CGDCONT) failed");
        return -1;
    }

    // Step 2: Activate context 1. Use long timeout (real networks can be slow).
    for (int attempt = 0; attempt < 2; attempt++) {
        n = modem_send_at(config, "AT+CGACT=1,1", resp, sizeof(resp), pdp_timeout);
        if (n >= 0 && response_contains(resp, "OK")) {
            config->state = MODEM_DRIVER_PDP_ACTIVE;
            return 0;
        }
        if (n < 0 || response_contains(resp, "+CME ERROR: 30")) {
            ESP_LOGE(TAG, "PDP activate failed (no network or timeout)");
            return -1;
        }
        if (response_contains(resp, "+CME ERROR: 33")) {
            ESP_LOGE(TAG, "PDP activate failed (not subscribed)");
            return -1;
        }
        if (response_contains(resp, "+CME ERROR: 148")) {
            ESP_LOGW(TAG, "PDP activate CME 148, retry %d", attempt + 1);
            continue;
        }
        ESP_LOGE(TAG, "PDP activate error");
        return -1;
    }
    ESP_LOGE(TAG, "PDP activate failed after retry");
    return -1;
}

// ---------------------------------------------------------------------------
//  modem_exit_data_mode
//
//  Sends +++ with guard time to return modem from PPP data mode to AT mode.
//  Required before retrying (redial/full_recovery), modem can process AT#99
// ---------------------------------------------------------------------------
static void modem_exit_data_mode(modem_driver_config_t *config)
{
    vTaskDelay(pdMS_TO_TICKS(1000));  // Guard time before +++
    uart_write_bytes(config->uart_num, "+++", 3);
    vTaskDelay(pdMS_TO_TICKS(500));   // Allow sim to detect timeout after +++
}

// ---------------------------------------------------------------------------
//  modem_enter_data_mode
//
//  Dials the data call (ATD*99#) to switch the UART from AT text to PPP
//  binary frames. We expect "CONNECT" (not "OK"). If we see "NO CARRIER" or
//  timeout, the PDP may have dropped — return -1. On success, set state
//  DATA_MODE; after this, no more AT commands — the link carries PPP.
// ---------------------------------------------------------------------------
int modem_enter_data_mode(modem_driver_config_t *config)
{
    char resp[256];

    TickType_t dial_timeout = pdMS_TO_TICKS(10000);

    int n = modem_send_at(config, "ATD*99#", resp, sizeof(resp), dial_timeout);
    if (n < 0) {
        ESP_LOGE(TAG, "Dial timeout");
        return -1;
    }
    if (response_contains(resp, "NO CARRIER")) {
        ESP_LOGE(TAG, "NO CARRIER — PDP dropped");
        return -1;
    }
    if (!response_contains(resp, "CONNECT")) {
        ESP_LOGE(TAG, "Dial failed (no CONNECT)");
        return -1;
    }
    config->state = MODEM_DRIVER_DATA_MODE;
    printf("PPP data mode established\n");
    return 0;
}

// ---------------------------------------------------------------------------
//  getIP_fails
//
//  Tracks consecutive PPP IP acquisition failures. Used by Phase 2.5 branch
//  logic: transient errors (LCP/IPCP timeout, config reject/nak) → goto redial;
//  link-down errors (LCP Terminate, 0.0.0.0, NO CARRIER) → goto full_recovery.
//  When getIP_fails > MAX_FAILS_GETIP we return fatal. 
// ---------------------------------------------------------------------------
static int getIP_fails = 0;

// PPP Phase 2: volatile flags set by event handlers, polled by main loop.
#define PPP_RX_BUF_SIZE 1024
#define PPP_IP_TIMEOUT_MS 30000

// What happened during PPP Phase 2 (LCP/IPCP negotiation after data mode).
typedef enum {
    PPP_POLL_NONE = 0,
    PPP_POLL_GOT_IP,
    PPP_POLL_FAIL_REDIAL,
    PPP_POLL_FAIL_FULL,
    PPP_POLL_FAIL_FATAL,
} ppp_poll_result_t;

// This changes asynchronously through PPP events
static volatile ppp_poll_result_t s_ppp_result = PPP_POLL_NONE;

// Action returned by modem_run_ppp_phase2 — tells caller where to go next.
typedef enum {
    PPP_PHASE2_IDLE = 0,
    PPP_PHASE2_ERROR,
    PPP_PHASE2_FULL_RECOVERY,
    PPP_PHASE2_REDIAL,
} ppp_phase2_action_t;

// Transmit callback: PPP stack calls this to send bytes to the modem.
// handle is our config pointer (set in driver_cfg).
// PPP stack (lwIP) uses this function to drive data thru UART (passed thru netif config)
static esp_err_t ppp_transmit(void *handle, void *buffer, size_t len)
{
    modem_driver_config_t *cfg = (modem_driver_config_t *)handle;
    uart_write_bytes(cfg->uart_num, buffer, len);
    return ESP_OK;
}

// Async handler function, posts event when PPP gets or loses an IP address
static void on_ppp_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "PPP got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_ppp_result = PPP_POLL_GOT_IP;
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGI(TAG, "PPP lost IP — link down");
        s_ppp_result = PPP_POLL_FAIL_FULL;
    }
}

// Async function, post when link fails or changes state
static void on_ppp_status_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    // Map NETIF_PPP_STATUS to redial vs full_recovery vs fatal (pseudocode 2.5).
    // redial: LCP timeout, config reject/nak, IPCP timeout, protocol/option mismatch
    // full_recovery: LCP Terminate, link dropped, 0.0.0.0, NO CARRIER
    if (id >= NETIF_PPP_INTERNAL_ERR_OFFSET) {
        ESP_LOGE(TAG, "PPP internal error %ld", (long)id);
        s_ppp_result = PPP_POLL_FAIL_FATAL;
        return;
    }
    if (id >= NETIF_PP_PHASE_OFFSET) {
        if (id == NETIF_PPP_PHASE_TERMINATE || id == NETIF_PPP_PHASE_DEAD ||
            id == NETIF_PPP_PHASE_DISCONNECT) {
            ESP_LOGW(TAG, "PPP phase %ld — link down, full recovery", (long)id);
            s_ppp_result = PPP_POLL_FAIL_FULL;
        }
        return;
    }
    switch ((int)id) {
        case NETIF_PPP_ERRORPROTOCOL:
        case NETIF_PPP_ERRORPARAM:
        case NETIF_PPP_ERRORPEERDEAD:
        case NETIF_PPP_ERRORIDLETIMEOUT:
        case NETIF_PPP_ERRORCONNECTTIME:
            ESP_LOGW(TAG, "PPP error %ld — transient, redial", (long)id);
            s_ppp_result = PPP_POLL_FAIL_REDIAL;
            break;
        case NETIF_PPP_ERRORCONNECT:
        case NETIF_PPP_ERROROPEN:
        case NETIF_PPP_ERRORDEVICE:
            ESP_LOGW(TAG, "PPP error %ld — link/device, full recovery", (long)id);
            s_ppp_result = PPP_POLL_FAIL_FULL;
            break;
        default:
            ESP_LOGE(TAG, "PPP error %ld — fatal", (long)id);
            s_ppp_result = PPP_POLL_FAIL_FATAL;
            break;
    }
}

// ---------------------------------------------------------------------------
//  modem_run_ppp_phase2
//
//  Step 3: PPP Phase 2 — hand UART to lwIP and bring up IP.
//  After modem_enter_data_mode() the UART carries binary PPP frames only.
//  No more AT; the driver must stop reading/writing this UART and let
//  the PPP stack own it. Following sim_modem.c commenting structure.
//
//  Returns action for caller: IDLE (success), ERROR, FULL_RECOVERY, or REDIAL.
//  Calls modem_exit_data_mode before returning FULL_RECOVERY or REDIAL.
// ---------------------------------------------------------------------------
static ppp_phase2_action_t modem_run_ppp_phase2(modem_driver_config_t *config)
{
    // Phase 2.1 — Create PPP network interface
    //   esp_netif_config_t cfg = ESP_NETIF_DEFAULT_PPP();
    //   esp_netif_t *netif = esp_netif_new(&cfg);
    esp_netif_inherent_config_t base_netif_cfg = ESP_NETIF_INHERENT_DEFAULT_PPP();
    base_netif_cfg.if_desc = "ppp_modem";
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)config,
        .transmit = ppp_transmit,
    };
    esp_netif_config_t netif_ppp_config = {
        .base = &base_netif_cfg,
        .driver = &driver_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_PPP,
    };
    //   If esp_netif_new fails (e.g. no heap). Check for NULL;
    //   log and goto error if so.
    esp_netif_t *netif = esp_netif_new(&netif_ppp_config);
    if (netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_new(PPP) failed — no heap?");
        return PPP_PHASE2_ERROR;
    }

    // Phase 2.2 — Document: PPPoS (PPP over serial) created in the previous step
    // should be bound to this UART, make sure nothing else is bound to it
    ESP_LOGI(TAG, "PPPoS created, bound to UART%d — nothing else uses this UART", config->uart_num);

    // Phase 2.3 — Register netif and set as default
    //   Register netif so lwIP routes through it. Set default so apps use this
    //   interface (not WiFi). Can go wrong: Netif not default — apps try WiFi.
    esp_netif_set_default_netif(netif);
    ESP_LOGI(TAG, "PPP netif registered and set as default");

    // Phase 2.4 — Document: lwIP (PPP state machine) does LCP and IPCP. ESP-IDF 
    // (PPPoS / netif glue), connects that to the UART and to the esp_netif. 
    // This bulk of this process happens backend, check result in 2.5
    s_ppp_result = PPP_POLL_NONE;

    // Registers IP_EVENT (on_ppp_ip_event) and NETIF_PPP_STATUS (on_ppp_status_event)
    // Provides handlers so they can be unregistered later
    esp_event_handler_instance_t ip_inst, status_inst;
    esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ppp_ip_event, NULL, &ip_inst);
    esp_event_handler_instance_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, on_ppp_status_event, NULL, &status_inst);

    // Starts and connect the network interface itself, preps PPP machine to run
    // Connects link layer, gives PPP serial link so it can being negotiation between ESP and Modem
    // Decide on MRU, authentication, compression, protocols
    esp_netif_action_start(netif, 0, 0, 0);
    esp_netif_action_connected(netif, 0, 0, 0);

    // Phase 2.5 — Wait for IP address (PPP got IP)
    //   - Register for IP_EVENT_PPP_GOT_IP (or equivalent) or block until
    //     esp_netif_get_ip_info(netif, &info) shows a valid address (not 0.0.0.0).
    //   - Can go wrong: Timeout — LCP/IPCP failed or modem didn't assign IP.
    //     Can go wrong: Event not fired — ensure netif and PPP are registered
    //     with the event loop.

    // Allocates a buffer for incoming PPP frames from UART
    char *ppp_buf = malloc(PPP_RX_BUF_SIZE);
    if (ppp_buf == NULL) {
        // Allocation failed, go error
        ESP_LOGE(TAG, "PPP RX buffer malloc failed");
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, ip_inst);
        esp_event_handler_instance_unregister(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, status_inst);
        esp_netif_action_disconnected(netif, 0, 0, 0);
        esp_netif_action_stop(netif, 0, 0, 0);
        esp_netif_destroy(netif);
        return PPP_PHASE2_ERROR;
    }

    // Use a timeout (e.g. 30s); if no IP by then, log "PPP IP timeout" and goto error.
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PPP_IP_TIMEOUT_MS);
    ppp_poll_result_t result = PPP_POLL_NONE;

    // Sequential polling loop — no ppp_rx_task, no race conditions.
    while (xTaskGetTickCount() < deadline) {
        // Read as bytes arrive from UART, stores up to its buffer size
        int n = uart_read_bytes(config->uart_num, ppp_buf, PPP_RX_BUF_SIZE,
                                pdMS_TO_TICKS(100));                  
        // Passed read bytes to netif, the IP is then stored in netif
        if (n > 0) {
            esp_netif_receive(netif, ppp_buf, (size_t)n, NULL);
        }
        // Holds the result of initial negotiation, ppp result set by event handlers
        result = s_ppp_result; 
        if (result != PPP_POLL_NONE) {
            // When result becomes a not none value, negotiation finished
            break; 
        }
    }

    if (result == PPP_POLL_GOT_IP) {
        // Phase 2.6 — Use the link: keep PPP netif alive, continuously feed UART to lwIP.
        // Other tasks can use standard socket APIs (connect, send, recv) — traffic routes
        // through the default PPP netif. Runs until link down (LOST_IP, PPP status events).
        getIP_fails = 0;
        config->state = MODEM_DRIVER_IP_UP;
        ESP_LOGI(TAG, "PPP IP acquisition success — Phase 2.6 link active");
        printf("PPP Phase 2 complete — IP up, link active (sockets usable)\n");

        s_ppp_result = PPP_POLL_NONE;  // Reset; Phase 2.6 watches for LOST_IP / status
        while (1) {
            int n = uart_read_bytes(config->uart_num, ppp_buf, PPP_RX_BUF_SIZE,
                                    pdMS_TO_TICKS(100));
            if (n > 0) {
                esp_netif_receive(netif, ppp_buf, (size_t)n, NULL);
            }
            result = s_ppp_result;
            if (result != PPP_POLL_NONE && result != PPP_POLL_GOT_IP) {
                break;  // Link down or error — exit Phase 2.6
            }
        }
        ESP_LOGW(TAG, "Phase 2.6 link down (result=%d) — cleaning up", (int)result);
    }

    // Cleanup: unregister handlers, tear down netif (on failure or Phase 2.6 link-down exit)
    free(ppp_buf);
    esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, ip_inst);
    esp_event_handler_instance_unregister(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, status_inst);
    esp_netif_action_disconnected(netif, 0, 0, 0);
    esp_netif_action_stop(netif, 0, 0, 0);
    esp_netif_destroy(netif);

    // If we reached here from Phase 2.6, result is FAIL_* (link down). Fall through to recovery.
    getIP_fails++;
    ESP_LOGW(TAG, "PPP IP failure (getIP_fails=%d)", getIP_fails);

    if (getIP_fails > MAX_FAILS_GETIP) {
        ESP_LOGE(TAG, "PPP IP failed %d times — fatal", getIP_fails);
        return PPP_PHASE2_ERROR;
    }

    if (result == PPP_POLL_FAIL_FATAL) {
        ESP_LOGE(TAG, "PPP fatal error");
        return PPP_PHASE2_ERROR;
    }

    if (result == PPP_POLL_FAIL_FULL) {
        modem_exit_data_mode(config);
        return PPP_PHASE2_FULL_RECOVERY;
    }

    if (result == PPP_POLL_FAIL_REDIAL) {
        modem_exit_data_mode(config);
        return PPP_PHASE2_REDIAL;
    }

    // Timeout: no IP after 30 seconds (pseudocode: no IP after 30 seconds)
    ESP_LOGW(TAG, "PPP IP timeout (30s)");
    modem_exit_data_mode(config);
    if (getIP_fails > MAX_FAILS_GETIP / 2) {
        return PPP_PHASE2_FULL_RECOVERY;
    }
    return PPP_PHASE2_REDIAL;
}

void modem_driver_task(void *param)
{
    modem_driver_config_t *config = (modem_driver_config_t *)param;

    ESP_LOGI(TAG, "modem_driver_task started");

    // -----------------------------------------------------------------------
    // Step 1: Wait for modem to boot.
    // The sim_modem_task boots in 1.5s. We wait 3s to be safe — on real
    // hardware, modems can take 5-15s. modem_check_sim() retries handle
    // the case where we start too early.
    // -----------------------------------------------------------------------
    printf("Driver: waiting 3 seconds for modem to boot...\n");
    vTaskDelay(pdMS_TO_TICKS(3000));
    printf("Driver: connecting to modem...\n");

    // -----------------------------------------------------------------------
    // Step 2: Run the connection sequence.
    // Each function returns 0 on success, -1 on failure.
    // On failure, skip remaining steps and report which step failed.
    // -----------------------------------------------------------------------

full_recovery:
    // Step 2a: Verify modem is alive and SIM is ready.
    // Sends: AT, ATE0, AT+CPIN? — expects OK and +CPIN: READY.
    if (modem_check_sim(config) != 0) {
        printf("=== Connection failed: SIM check ===\n");
        goto error;
    }
    printf("Driver: SIM OK\n");

    // Step 2b: Register on the cellular network.
    // Sends: AT+CSQ, AT+CREG?, AT+COPS? — waits for registration.
    if (modem_register_network(config) != 0) {
        printf("=== Connection failed: network registration ===\n");
        goto error;
    }
    printf("Driver: Registered on network\n");

    // Step 2c: Activate PDP context (data bearer).
    // Sends: AT+CGDCONT, AT+CGACT — establishes IP path through carrier.
    if (modem_activate_pdp(config) != 0) {
        printf("=== Connection failed: PDP activation ===\n");
        goto error;
    }
    printf("Driver: PDP context active\n");

redial:
    // Step 2d: Switch UART from AT text to PPP binary.
    // Sends: ATD*99# — expects CONNECT. After this, no more AT commands.
    if (modem_enter_data_mode(config) != 0) {
        printf("=== Connection failed: data mode ===\n");
        goto error;
    }
    printf("Driver: PPP link up\n");

    // -----------------------------------------------------------------------
    // Step 3: PPP Phase 2 — hand UART to lwIP and bring up IP.
    // -----------------------------------------------------------------------
    switch (modem_run_ppp_phase2(config)) {
        case PPP_PHASE2_IDLE:
            goto idle;
        case PPP_PHASE2_ERROR:
            goto error;
        case PPP_PHASE2_FULL_RECOVERY:
            goto full_recovery;
        case PPP_PHASE2_REDIAL:
            goto redial;
    }

error:
    config->state = MODEM_DRIVER_ERROR;
    printf("Driver: stopped due to error (state=%d)\n", config->state);

idle:
    // Task must never return — sit in a delay loop.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
