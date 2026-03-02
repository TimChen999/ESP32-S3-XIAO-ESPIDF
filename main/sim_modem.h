#pragma once

#include "driver/uart.h"

// ============================================================================
//  SIMULATED MODEM — Header
//
//  This module pretends to be a cellular modem (e.g. SIM7600, BG96, SARA-R4).
//  It listens on its UART for AT commands from the driver, parses them, and
//  sends back realistic responses. In a real system, this entire file is
//  replaced by the physical modem hardware.
//
//  The modem progresses through a state machine:
//    OFF → READY → SIM_READY → REGISTERED → PDP_ACTIVE → DATA_MODE(PPP)
//
// ============================================================================
//  WHERE THIS FITS IN THE SYSTEM
// ============================================================================
//
//  In a real cellular device, the modem is a separate chip (or module) wired
//  to the MCU over UART. It owns everything below the AT command interface:
//  the radio, baseband processor, SIM card reader, and the cellular protocol
//  stack (RRC, NAS, etc). The MCU never sees any of that — it only sees AT
//  commands and, later, PPP frames.
//
//  This file simulates that entire module. It doesn't simulate the radio or
//  cellular internals (those are irrelevant to the MCU firmware), but it
//  does faithfully reproduce the UART-visible behavior: the AT responses,
//  the timing, the state transitions, and eventually the PPP data pipe.
//
//  ┌────────────────────────────────────────────────────────────────────┐
//  │                     NETWORKING LAYER MAP                          │
//  ├──────────────┬─────────────────────────────────────────────────────┤
//  │ Layer 7      │ Application (HTTP, MQTT, CoAP)                     │
//  │ (App)        │   → Lives in modem_driver / app code               │
//  ├──────────────┼─────────────────────────────────────────────────────┤
//  │ Layer 4      │ TCP / UDP                                          │
//  │ (Transport)  │   → lwIP on the driver side                        │
//  │              │   → sim_modem fakes the remote endpoint             │
//  ├──────────────┼─────────────────────────────────────────────────────┤
//  │ Layer 3      │ IP                                                 │
//  │ (Network)    │   → lwIP on the driver side                        │
//  │              │   → sim_modem assigns IP via IPCP, routes packets   │
//  ├──────────────┼─────────────────────────────────────────────────────┤
//  │ Layer 2      │ PPP (Point-to-Point Protocol)                      │
//  │ (Data Link)  │   → HDLC framing over UART                        │
//  │              │   → LCP / IPCP negotiation                         │
//  │              │   ★ THIS IS THE BOUNDARY between driver & modem ★  │
//  │              │   → Driver side: lwIP PPP client                    │
//  │              │   → sim_modem side: PPP server (handle_ppp_data)    │
//  ├──────────────┼─────────────────────────────────────────────────────┤
//  │ Layer 1      │ Physical: UART electrical signals                  │
//  │ (Physical)   │   → Real GPIO pins, real baud rate, real bytes     │
//  │              │   → Cross-wired: Driver TX→Modem RX, Modem TX→     │
//  │              │     Driver RX                                      │
//  ├──────────────┼─────────────────────────────────────────────────────┤
//  │ Control      │ AT Command Interface (pre-PPP)                     │
//  │ Plane        │   → Not a networking layer per se, but the         │
//  │              │     out-of-band control channel that configures     │
//  │              │     the modem before data flows.                    │
//  │              │   → Handles: SIM auth, network registration,       │
//  │              │     PDP context, signal quality, operator select    │
//  │              │   → sim_modem: parses & responds (handle_at_cmd)   │
//  │              │   → modem_driver: sends & validates (modem_send_at)│
//  └──────────────┴─────────────────────────────────────────────────────┘
//
//  WHAT sim_modem OWNS:
//    - Layer 1 (one end of the physical UART)
//    - Control plane (AT command responses)
//    - Layer 2 server side (PPP/HDLC framing, LCP/IPCP negotiation)
//    - Layer 3 termination (IP address assignment, packet routing)
//    - Layer 4 fake endpoints (TCP handshake, UDP echo, DNS responses)
//
//  In production, you DELETE this file and plug in a real modem. The driver
//  code (modem_driver.c) doesn't change at all.
//
// ============================================================================

typedef enum {
    SIM_MODEM_STATE_OFF,
    SIM_MODEM_STATE_READY,         // Powered on, responds to AT
    SIM_MODEM_STATE_SIM_READY,     // SIM card detected
    SIM_MODEM_STATE_REGISTERED,    // Registered on (fake) network
    SIM_MODEM_STATE_PDP_ACTIVE,    // PDP context activated, IP assigned
    SIM_MODEM_STATE_DATA_MODE,     // PPP data mode active
} sim_modem_state_t;

typedef struct {
    uart_port_t uart_num;          // Which UART this modem listens on
    int tx_pin;
    int rx_pin;
    int rts_pin;                   // RTS output (or UART_PIN_NO_CHANGE if disabled)
    int cts_pin;                   // CTS input  (or UART_PIN_NO_CHANGE if disabled)
    int baud_rate;
    bool flow_control;             // true = enable RTS/CTS hardware flow control
    sim_modem_state_t state;       // Current state in the lifecycle
} sim_modem_config_t;

// ---------------------------------------------------------------------------
//  sim_modem_init
//
//  Called once before starting the task.
//  TODO:
//    1. Configure the UART peripheral (uart_num) with the given baud/pins
//    2. Install the UART driver with appropriate RX/TX buffer sizes
//    3. Set initial state to SIM_MODEM_STATE_OFF
// ---------------------------------------------------------------------------
void sim_modem_init(sim_modem_config_t *config);

// ---------------------------------------------------------------------------
//  sim_modem_task
//
//  The main FreeRTOS task entry point. Runs forever.
//  This is what main.c passes to xTaskCreatePinnedToCore().
//
//  Parameter: pointer to sim_modem_config_t
//
//  TODO — high-level flow:
//    1. Transition from OFF → READY (simulate a short power-on delay)
//    2. Enter main loop:
//       a. Read a line from UART RX (block until '\r' or '\n')
//       b. Pass the line to the AT command parser
//       c. Send the response back on UART TX
//    3. When ATD*99# is received and state is PDP_ACTIVE:
//       a. Send "CONNECT 115200\r\n"
//       b. Transition to DATA_MODE
//       c. Enter the PPP handler loop (Phase 2 — implement later)
// ---------------------------------------------------------------------------
void sim_modem_task(void *param);
