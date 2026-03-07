#pragma once

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"

// ============================================================================
//  MODEM DRIVER — Header
//
//  This module talks to a modem over UART using AT commands.
//  On real hardware, the UART connects to a physical modem (SIM7600, BG96, etc).
//  In simulation, the UART connects to sim_modem running on the other UART.
//
//  THIS FILE STAYS THE SAME whether you're talking to real or simulated hardware.
//  That's the whole point — driver code is hardware-agnostic beyond UART I/O.
//
//  Lifecycle driven by the application:
//    init → check_sim → register_network → activate_pdp → enter_data_mode
//
// ============================================================================
//  WHERE THIS FITS IN THE SYSTEM
// ============================================================================
//
//  The modem driver is the MCU's interface to the cellular network. It sits
//  between application logic (which wants to do things like "HTTP GET") and
//  the raw UART link to the modem hardware. It operates in two phases:
//
//  PHASE 1 — AT COMMAND PHASE (control plane)
//    The driver sends text-based AT commands and parses text responses.
//    This phase configures the modem: checks the SIM card, registers on the
//    network, activates a data bearer (PDP context), and requests PPP mode.
//    No user data (IP packets) flows during this phase.
//
//    The driver functions map to this control flow:
//      modem_check_sim()        → "Is the modem on? Is the SIM valid?"
//      modem_register_network() → "Are we connected to a cell tower?"
//      modem_activate_pdp()     → "Give us a data channel (IP bearer)"
//      modem_enter_data_mode()  → "Switch UART from AT text to PPP binary"
//
//  PHASE 2 — PPP DATA PHASE (data plane)
//    After ATD*99#, the UART no longer carries AT commands. Instead it
//    carries PPP frames (HDLC-framed binary). The driver hands the UART
//    to lwIP's PPP client, which negotiates LCP/IPCP and brings up an IP
//    interface. From that point, standard socket APIs (connect, send, recv)
//    work transparently — TCP/IP packets get wrapped in PPP frames and
//    sent over the UART to the modem, which forwards them to the network.
//
//  ┌────────────────────────────────────────────────────────────────────┐
//  │                     NETWORKING LAYER MAP                           │
//  ├──────────────┬─────────────────────────────────────────────────────┤
//  │ Layer 7      │ Application (HTTP, MQTT, CoAP)                      │
//  │ (App)        │   → Your app code, calls socket APIs                │
//  │              │   → Runs AFTER this driver brings the link up       │
//  ├──────────────┼─────────────────────────────────────────────────────┤
//  │ Layer 4      │ TCP / UDP                                           │
//  │ (Transport)  │   → lwIP handles this automatically                 │
//  │              │   → You call connect(), send(), recv()              │
//  ├──────────────┼─────────────────────────────────────────────────────┤
//  │ Layer 3      │ IP                                                  │
//  │ (Network)    │   → lwIP handles this automatically                 │
//  │              │   → IP address assigned during IPCP negotiation     │
//  ├──────────────┼─────────────────────────────────────────────────────┤
//  │ Layer 2      │ PPP (Point-to-Point Protocol)                       │
//  │ (Data Link)  │   → lwIP PPP client handles HDLC framing            │
//  │              │   → Negotiates LCP (link params) and IPCP (IP addr) │
//  │              │   ★ modem_enter_data_mode() triggers this phase ★  │
//  ├──────────────┼─────────────────────────────────────────────────────┤
//  │ Layer 1      │ Physical: UART                                      │
//  │ (Physical)   │   → modem_driver_init() sets up this layer          │
//  │              │   → modem_send_at() reads/writes raw bytes here     │
//  ├──────────────┼─────────────────────────────────────────────────────┤
//  │ Control      │ AT Command Interface (pre-PPP)                      │
//  │ Plane        │   ★ THIS IS WHERE MOST OF THIS FILE OPERATES ★     │
//  │              │   → modem_send_at(): lowest-level AT send/receive   │
//  │              │   → modem_check_sim(): SIM/modem validation         │
//  │              │   → modem_register_network(): cellular registration │
//  │              │   → modem_activate_pdp(): IP bearer setup           │
//  │              │   → The control plane is NOT a networking layer —   │
//  │              │     it's the out-of-band management channel that    │
//  │              │     brings up Layers 1-3 before data can flow.      │
//  └──────────────┴─────────────────────────────────────────────────────┘
//
//  WHAT modem_driver OWNS:
//    - Layer 1 setup (UART configuration)
//    - Control plane (AT command sequence to bring the modem online)
//    - Layer 2 handoff (triggers PPP by entering data mode)
//    - Layers 2-4 are then handled by lwIP (not this file)
//    - Layer 7 is handled by your application code (not this file)
//
//  This file is the SAME code you'd ship on a real product. Nothing in
//  here knows or cares that sim_modem.c exists on the other UART.
//
// ============================================================================
//  HOW OTHER FILES INTERFACE WITH THIS DRIVER (Phase 2.6)
// ============================================================================
//
//  Once modem_driver_task reaches Phase 2.6, the PPP netif is the default
//  interface. Any code (in other tasks or files) can use standard APIs:
//
//  1. CHECK LINK STATE (optional):
//       if (modem_driver_get_state() == MODEM_DRIVER_IP_UP) { ... }
//
//  2. SOCKETS (TCP/UDP):
//       #include <sys/socket.h>
//       #include <netdb.h>
//       int fd = socket(AF_INET, SOCK_STREAM, 0);
//       struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(443), ... };
//       inet_pton(AF_INET, "142.250.80.46", &addr.sin_addr);  // or use getaddrinfo for DNS
//       connect(fd, (struct sockaddr *)&addr, sizeof(addr));
//       send(fd, data, len, 0);
//       recv(fd, buf, sizeof(buf), 0);
//
//  3. DNS:
//       struct hostent *he = gethostbyname("generativelanguage.googleapis.com");
//       // Then use he->h_addr_list[0] in sockaddr_in
//
//  4. HTTP (esp_http_client component):
//       #include "esp_http_client.h"
//       esp_http_client_config_t cfg = { .url = "https://example.com", ... };
//       esp_http_client_handle_t client = esp_http_client_init(&cfg);
//       esp_http_client_perform(client);  // GET by default
//       esp_http_client_cleanup(client);
//
//  5. HTTPS / TLS (Gemini API, etc.):
//       Use esp_http_client with .url = "https://..." — ESP-IDF uses mbedTLS.
//       For Gemini: POST to https://generativelanguage.googleapis.com/... with
//       JSON body and API key in header.
//
//  6. MQTT, CoAP, custom protocols:
//       Same pattern — create sockets or use ESP-IDF components; traffic
//       automatically routes through the default PPP netif.
//
//  The modem_driver_task runs the PPP RX loop; your code runs in other tasks.
//  Ensure modem_driver_get_state() == MODEM_DRIVER_IP_UP before using the network.
//
// ============================================================================

typedef enum {
    MODEM_DRIVER_IDLE,
    MODEM_DRIVER_SIM_OK,
    MODEM_DRIVER_REGISTERED,
    MODEM_DRIVER_PDP_ACTIVE,
    MODEM_DRIVER_DATA_MODE,
    MODEM_DRIVER_IP_UP,       // PPP link up, IP assigned — sockets/DNS/TLS usable
    MODEM_DRIVER_ERROR,
} modem_driver_state_t;

typedef struct {
    uart_port_t uart_num;          // Which UART the modem is on
    int tx_pin;
    int rx_pin;
    int rts_pin;                   // RTS output (or UART_PIN_NO_CHANGE if disabled)
    int cts_pin;                   // CTS input  (or UART_PIN_NO_CHANGE if disabled)
    int baud_rate;
    bool flow_control;             // true = enable RTS/CTS hardware flow control
    modem_driver_state_t state;
} modem_driver_config_t;

// ---------------------------------------------------------------------------
//  modem_driver_get_state
//
//  Returns the current modem driver state. Use MODEM_DRIVER_IP_UP to check
//  if the link is up and sockets are usable. Returns MODEM_DRIVER_IDLE if
//  the driver has not been initialized.
// ---------------------------------------------------------------------------
modem_driver_state_t modem_driver_get_state(void);

// ---------------------------------------------------------------------------
//  modem_driver_init
//
//  Called once before starting the task. Configures the UART peripheral,
//  installs the driver, and sets state to IDLE.  Pin assignments and UART
//  parameters are defined inside modem_driver.c.  On real hardware only
//  this side exists (the sim side wouldn't exist).
// ---------------------------------------------------------------------------
void modem_driver_init(void);

// ---------------------------------------------------------------------------
//  modem_send_at
//
//  Sends an AT command string and waits for a response.
//
//  TODO:
//    1. Append '\r' to the command and send via uart_write_bytes()
//    2. Read the response line(s) using uart_read_bytes() with timeout
//    3. Store response in resp_buf, null-terminate
//    4. Return number of bytes read, or -1 on timeout
//
//  This is the lowest-level building block. All higher-level functions
//  (check_sim, register, etc.) call this.
// ---------------------------------------------------------------------------
int modem_send_at(modem_driver_config_t *config,
                  const char *cmd,
                  char *resp_buf, size_t resp_buf_size,
                  TickType_t timeout);

// ---------------------------------------------------------------------------
//  modem_check_sim
//
//  Verifies the modem is alive and the SIM is ready.
//
//  TODO:
//    1. Send "AT" — expect "OK" (proves modem is alive)
//       - If no response, retry a few times (modem may still be booting)
//    2. Send "ATE0" — expect "OK" (disable echo)
//    3. Send "AT+CPIN?" — expect "+CPIN: READY"
//    4. If all succeed, set state = MODEM_DRIVER_SIM_OK, return 0
//    5. If any fail, set state = MODEM_DRIVER_ERROR, return -1
// ---------------------------------------------------------------------------
int modem_check_sim(modem_driver_config_t *config);

// ---------------------------------------------------------------------------
//  modem_register_network
//
//  Waits for the modem to register on the cellular network.
//
//  TODO:
//    1. Send "AT+CSQ" — log the signal quality from the response
//    2. Send "AT+CREG?" — check for "+CREG: 0,1" (registered, home)
//       - If "+CREG: 0,2" (searching), wait and retry
//    3. Send "AT+COPS?" — log the operator name
//    4. If registered, set state = MODEM_DRIVER_REGISTERED, return 0
//    5. If not registered after N retries, return -1
// ---------------------------------------------------------------------------
int modem_register_network(modem_driver_config_t *config);

// ---------------------------------------------------------------------------
//  modem_activate_pdp
//
//  Sets up the PDP context (data connection).
//
//  TODO:
//    1. Send "AT+CGDCONT=1,\"IP\",\"internet\"" — define PDP context
//       - Expect "OK"
//    2. Send "AT+CGACT=1,1" — activate PDP context
//       - Expect "OK"
//    3. Set state = MODEM_DRIVER_PDP_ACTIVE, return 0
//    4. On failure, return -1
// ---------------------------------------------------------------------------
int modem_activate_pdp(modem_driver_config_t *config);

// ---------------------------------------------------------------------------
//  modem_enter_data_mode
//
//  Switches the modem from AT command mode to PPP data mode.
//
//  TODO:
//    1. Send "ATD*99#"
//    2. Wait for "CONNECT" response (not just "OK")
//    3. Set state = MODEM_DRIVER_DATA_MODE, return 0
//    4. After this returns, the UART carries raw PPP frames — no more AT.
//       The caller should hand the UART off to a PPP/lwIP stack.
// ---------------------------------------------------------------------------
int modem_enter_data_mode(modem_driver_config_t *config);

// ---------------------------------------------------------------------------
//  modem_driver_task
//
//  The main FreeRTOS task entry point. Runs the full connection sequence.
//  This is what main.c passes to xTaskCreatePinnedToCore().
//
//  Parameter: pointer to modem_driver_config_t
//
//  TODO — high-level flow:
//    1. Wait a moment for the modem to boot (the sim_modem needs time too)
//    2. Call modem_check_sim()     — verify modem + SIM
//    3. Call modem_register_network() — wait for registration
//    4. Call modem_activate_pdp()  — establish data bearer
//    5. Call modem_enter_data_mode() — switch to PPP
//    6. (Phase 2) Hand UART to lwIP PPP client for IP connectivity
//    7. Log success/failure at each step via printf() to UART0 (console)
//
//  On any failure, log the error and either retry or stop.
// ---------------------------------------------------------------------------
void modem_driver_task(void *param);
