# Cellular Modem Simulator — FSM & Networking Layer Guide

This document walks through the complete finite state machine (FSM) of the
cellular modem simulator. By understanding the FSM, you will also understand
the networking layers involved — each state transition corresponds to a
specific layer or protocol doing its job.

---

## System Overview

Two FSMs run in parallel on the ESP32-S3, connected by a physical UART link:

```
  ┌──────────────────────┐         UART wires          ┌──────────────────────┐
  │    DRIVER FSM        │  Driver TX ──── Modem RX    │    MODEM FSM         │
  │    (modem_driver.c)  │  Driver RX ──── Modem TX    │    (sim_modem.c)     │
  │    Core 1            │                              │    Core 0            │
  └──────────────────────┘                              └──────────────────────┘
```

The driver FSM **sends** AT commands and interprets responses.
The modem FSM **receives** AT commands and generates responses.

They advance through their states in lockstep — the driver can't move to the
next state until the modem responds correctly to the current command.

---

## The Two FSMs Side by Side

```
         DRIVER FSM                              MODEM FSM
         (modem_driver.c)                        (sim_modem.c)
         ──────────────                          ─────────────

         ┌─────────┐                             ┌─────────┐
         │  IDLE   │                             │   OFF   │
         └────┬────┘                             └────┬────┘
              │ (wait 3s for modem boot)              │ (1-2s boot delay)
              │                                       │
              │                                       ▼
              │                                  ┌─────────┐
              │                                  │  READY  │
              │                                  └────┬────┘
              │        AT\r ─────────────────────►    │
              │        ◄───────────────────── OK\r\n  │
              │        ATE0\r ───────────────────►    │
              │        ◄───────────────────── OK\r\n  │
              │        AT+CPIN?\r ───────────────►    │
              │        ◄── +CPIN: READY\r\nOK\r\n    │
              │                                       ▼
              ▼                                  ┌──────────┐
         ┌─────────┐                             │ SIM_READY│
         │ SIM_OK  │                             └────┬─────┘
         └────┬────┘                                  │
              │        AT+CSQ\r ─────────────────►    │
              │        ◄── +CSQ: 20,0\r\nOK\r\n      │
              │        AT+CREG?\r ───────────────►    │
              │        ◄── +CREG: 0,1\r\nOK\r\n      │
              │        AT+COPS?\r ───────────────►    │
              │        ◄── +COPS: 0,0,"Fake.."\r\n   │
              │                                       ▼
              ▼                                  ┌────────────┐
         ┌────────────┐                          │ REGISTERED │
         │ REGISTERED │                          └─────┬──────┘
         └─────┬──────┘                                │
               │     AT+CGDCONT=1,"IP","internet"\r ──►│
               │     ◄────────────────────── OK\r\n    │
               │     AT+CGACT=1,1\r ──────────────────►│
               │     ◄────────────────────── OK\r\n    │
               │                                       ▼
               ▼                                 ┌────────────┐
         ┌────────────┐                          │ PDP_ACTIVE │
         │ PDP_ACTIVE │                          └─────┬──────┘
         └─────┬──────┘                                │
               │     ATD*99#\r ───────────────────────►│
               │     ◄──────────── CONNECT 115200\r\n  │
               │                                       ▼
               ▼                                 ┌───────────┐
         ┌───────────┐                           │ DATA_MODE │
         │ DATA_MODE │                           └───────────┘
         └───────────┘
              ║                                       ║
              ║     UART now carries binary PPP       ║
              ║     frames instead of AT text         ║
              ▼                                       ▼
         ┌────────────────────────────────────────────────┐
         │              PHASE 2: PPP / IP                 │
         │         (see PPP FSM section below)            │
         └────────────────────────────────────────────────┘
```

---

## Hardware Flow Control (RTS/CTS) — Optional Layer 1 Enhancement

Flow control is a Layer 1 mechanism that prevents data loss when one side
can't process incoming bytes fast enough. It's optional — the system works
without it — but on real hardware at high baud rates or during heavy PPP
traffic, it prevents FIFO overruns.

### How RTS/CTS Works

```
  DRIVER (UART1)                                   MODEM (UART2)
  ┌────────────┐                                   ┌────────────┐
  │         TX ├──────────── data ────────────────►│ RX         │
  │         RX │◄──────────── data ────────────────┤ TX         │
  │            │                                   │            │
  │        RTS ├──── "I have room, send" ─────────►│ CTS        │
  │        CTS │◄──── "I have room, send" ─────────┤ RTS        │
  └────────────┘                                   └────────────┘

  RTS = Request To Send  (OUTPUT — "I'm ready to receive")
  CTS = Clear To Send    (INPUT  — "the other side is ready to receive")
```

Each UART's RTS output connects to the OTHER UART's CTS input. **Cross them,
don't run them straight.** RTS→RTS is wrong and will block immediately.

### Default State at Power-On

When the UART peripheral is configured with `UART_HW_FLOWCTRL_CTS_RTS`:

- **RTS starts LOW (asserted = "I'm ready")** — the RX FIFO is empty, which
  is below `rx_flow_ctrl_thresh`, so the hardware says "send me data."
- **CTS reads the other side's RTS** — if wired correctly, the other side's
  RTS is also LOW, so CTS sees "ready" and TX proceeds immediately.

Both sides start ready. Data flows immediately. **Flow control is permissive
by default** — "send until I tell you to stop."

### When RTS Blocks

```
  ┌─────────────────────────────────────────────────────┐
  │  RX FIFO                                             │
  │  ┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐         │
  │  │ ■ │ ■ │ ■ │ ■ │ ■ │ ■ │ ■ │   │   │   │  70%    │ RTS = LOW (ready)
  │  └───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘         │
  │                                                      │
  │  ┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐         │
  │  │ ■ │ ■ │ ■ │ ■ │ ■ │ ■ │ ■ │ ■ │ ■ │ ■ │  95%    │ RTS = HIGH (stop!)
  │  └───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘         │
  │              ▲ rx_flow_ctrl_thresh                    │
  │                                                      │
  │  ┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐         │
  │  │ ■ │ ■ │ ■ │   │   │   │   │   │   │   │  30%    │ RTS = LOW (ready)
  │  └───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘         │ (code read data)
  └─────────────────────────────────────────────────────┘
```

The threshold (`rx_flow_ctrl_thresh`) is the FIFO fill level that triggers
backpressure. Set it to ~122 for the default 128-byte FIFO — that gives 6
bytes of margin for in-flight data.

### When to Enable Flow Control

| Scenario | Flow control needed? | Why |
|---|---|---|
| Phase 1: AT commands | No | Small, infrequent messages. FIFO never fills up. |
| Phase 2: PPP at 115200 | Probably no | lwIP reads data promptly. Unlikely to overrun. |
| Phase 2: PPP at 921600+ | **Yes** | High throughput can overrun FIFO if the task is preempted. |
| Heavy TCP traffic | **Yes** | Bursts of back-to-back PPP frames during large transfers. |
| Real hardware (production) | **Yes** | Real modems can burst faster than the MCU reads. Always use it. |

**Recommendation:** Start with flow control disabled (`UART_HW_FLOWCTRL_DISABLE`)
to keep debugging simple. Enable it once basic AT commands and PPP work
correctly. Flow control never changes the data — it only changes the *timing*
of when bytes flow. If your code works without it, it will work with it.

### Pin Assignments (when enabled)

These are defined in `main.c` alongside the TX/RX pins:

```
Driver UART1:    RTS = D2 (GPIO3)     Modem UART2:    RTS = D4 (GPIO5)
                 CTS = D3 (GPIO4)                      CTS = D9 (GPIO8)

Cross-wiring:
  Driver RTS (D2) ──────► Modem CTS  (D9)    "Driver ready"  → Modem checks before TX
  Modem RTS  (D4) ──────► Driver CTS (D3)    "Modem ready"   → Driver checks before TX
```

### Troubleshooting: Blocked Immediately After Enabling

If the UART blocks and no data flows after enabling RTS/CTS:

1. **Check wiring order** — RTS must cross to CTS. RTS→RTS is wrong.
2. **Check the pin** — is CTS reading a floating/disconnected GPIO? A floating
   pin may read HIGH = "not ready" = TX blocked forever.
3. **Test one direction** — set `flow_ctrl = UART_HW_FLOWCTRL_RTS` (RTS only,
   no CTS checking) on one side. If that direction flows, the CTS wiring on
   that side was the problem.
4. **Check for inversion** — the ESP32-S3 GPIO matrix can invert signals. If
   you're touching GPIO matrix registers directly (not just `uart_set_pin()`),
   you may have accidentally inverted the CTS input. LOW should mean "ready."

### Config Struct Changes

When flow control is enabled, the config structs in `main.c` gain two
additional pin fields (`rts_pin`, `cts_pin`) and `sim_modem_config_t` /
`modem_driver_config_t` gain matching struct members. The init functions
pass these to `uart_set_pin()` instead of `UART_PIN_NO_CHANGE`, and set
`.flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS` with `.rx_flow_ctrl_thresh = 122`.

---

## Phase 1: AT Command Phase — State by State

### State 0: OFF / IDLE (Power-On)

```
Networking layer:  Layer 1 — Physical
What happens:      UART hardware initializes. GPIO pins configured.
                   Electrical signals become valid.
                   Flow control (if enabled): RTS asserted LOW on both
                   sides — both ready to receive immediately.
```

| | Driver (IDLE) | Modem (OFF) |
|---|---|---|
| **Entry condition** | `app_main()` calls `modem_driver_init()` | `app_main()` calls `sim_modem_init()` |
| **What to implement** | Configure UART1 (baud, pins, buffers). Set state = IDLE. | Configure UART2 (baud, pins, buffers). Set state = OFF. |
| **Inputs** | Config struct with UART num, pins, baud rate | Config struct with UART num, pins, baud rate |
| **Outputs** | UART1 peripheral is live, TX/RX pins are active | UART2 peripheral is live, TX/RX pins are active |
| **What you learn** | Layer 1 is about configuring the physical medium — the wire, the baud rate, and optionally flow control. No data flows yet. Same as plugging in an Ethernet cable or powering on a radio. |

**Transition trigger:** Task starts → modem delays 1-2s (simulating boot) → modem moves to READY.
Driver delays 3s (waiting for modem boot) then starts sending commands.

---

### State 1: READY / SIM Check

```
Networking layer:  Control Plane (out-of-band management)
What happens:      Driver verifies the modem is alive and the SIM card is valid.
                   This is like checking that your network adapter exists
                   before trying to connect to a network.
```

| | Driver sends | Modem receives & responds |
|---|---|---|
| **Step 1** | `"AT\r"` | Parses `"AT"` → responds `"OK\r\n"` |
| **Step 2** | `"ATE0\r"` | Parses `"ATE0"` → responds `"OK\r\n"` |
| **Step 3** | `"AT+CPIN?\r"` | Parses `"AT+CPIN?"` → responds `"+CPIN: READY\r\nOK\r\n"` |

| | Driver | Modem |
|---|---|---|
| **Function** | `modem_check_sim()` | `handle_at_command()` |
| **Input** | Nothing (initiates the exchange) | AT command string from UART RX |
| **Output** | Parsed response string, success/fail return code | Response string written to UART TX |
| **State before** | IDLE | READY |
| **State after (success)** | SIM_OK | SIM_READY |
| **State after (failure)** | ERROR | (stays READY, waits for retry) |

**What each command does:**

- `AT` — The simplest possible command. Just asks "are you there?" The modem
  replies "OK" if it's alive. This is like a Layer 1 loopback test — can
  bytes travel the wire and come back?

- `ATE0` — Tells the modem to stop echoing commands back. By default, modems
  echo every character you send (you'd see your own command in the response).
  ATE0 disables this, so you only see the modem's actual response.

- `AT+CPIN?` — Asks about the SIM card. The SIM (Subscriber Identity Module)
  holds your cellular identity (IMSI), encryption keys, and phone number.
  Without a valid SIM, the modem can't authenticate to any network.
  Response `+CPIN: READY` means the SIM is present and unlocked.
  On a real modem, you might get `+CPIN: SIM PIN` if the SIM requires a PIN code.

**Error handling to implement:**
- If `AT` gets no response, the modem may still be booting. Retry up to 5 times
  with 1-second delays between attempts.
- If `AT+CPIN?` returns anything other than `READY`, the SIM is missing or locked.
  This is a fatal error — you can't proceed without a SIM.

---

### State 2: REGISTERED / Network Registration

```
Networking layer:  Still Control Plane, but now touching Layer 2/3 concepts.
What happens:      The modem searches for a cell tower, authenticates with
                   the network, and registers. This is the cellular equivalent
                   of associating with a WiFi access point — you're now "on"
                   the network, but you don't have an IP address yet.
```

| | Driver sends | Modem receives & responds |
|---|---|---|
| **Step 1** | `"AT+CSQ\r"` | Responds `"+CSQ: 20,0\r\nOK\r\n"` |
| **Step 2** | `"AT+CREG?\r"` | Responds `"+CREG: 0,1\r\nOK\r\n"` |
| **Step 3** | `"AT+COPS?\r"` | Responds `"+COPS: 0,0,\"Fake Cellular\"\r\nOK\r\n"` |

| | Driver | Modem |
|---|---|---|
| **Function** | `modem_register_network()` | `handle_at_command()` |
| **Input** | Nothing (initiates) | AT command string |
| **Output** | Signal quality, registration status, operator name | Response strings |
| **State before** | SIM_OK | SIM_READY |
| **State after** | REGISTERED | REGISTERED |

**What each command does:**

- `AT+CSQ` — Query signal strength. Response `+CSQ: rssi,ber` where:
  - `rssi`: 0-31 scale (higher=better). 20 ≈ -73 dBm (good signal).
    99 = unknown/no signal.
  - `ber`: bit error rate, 0-7 scale. 0 = lowest error rate.

  **Networking parallel:** This is like checking WiFi signal bars. You can be
  near a tower (strong signal) or far away (weak signal). Weak signal means
  more retransmissions, slower throughput, and potential disconnects.

- `AT+CREG?` — Query network registration status. Response `+CREG: n,stat`:
  - `stat=0`: Not registered, not searching
  - `stat=1`: Registered, home network
  - `stat=2`: Not registered, currently searching
  - `stat=3`: Registration denied
  - `stat=5`: Registered, roaming

  **Networking parallel:** This is the cellular version of 802.11 association.
  The modem and cell tower perform mutual authentication (using keys from
  the SIM card) and the network accepts or rejects the device. Once
  registered, the modem has a radio channel but no IP connectivity yet.

  **What to implement:** If stat=2 (searching), wait 2 seconds and poll again.
  In a real system, registration can take 5-30 seconds depending on network
  conditions. Retry up to 10 times before giving up.

- `AT+COPS?` — Query which operator we're registered with. Response includes
  the network name ("Fake Cellular" in sim, real ones: "T-Mobile", "Verizon",
  "Vodafone"). This is informational — log it for debugging.

---

### State 3: PDP_ACTIVE / Data Bearer Setup

```
Networking layer:  Layer 3 — Network (IP address assignment begins here)
What happens:      A PDP (Packet Data Protocol) context is activated.
                   This creates a data "tunnel" through the cellular network
                   and assigns an IP address to the device. Think of it as
                   DHCP but for cellular — you go from "registered on the
                   network" to "I have an IP address and can send packets."
```

| | Driver sends | Modem receives & responds |
|---|---|---|
| **Step 1** | `"AT+CGDCONT=1,\"IP\",\"internet\"\r"` | Responds `"OK\r\n"` |
| **Step 2** | `"AT+CGACT=1,1\r"` | Responds `"OK\r\n"` |

| | Driver | Modem |
|---|---|---|
| **Function** | `modem_activate_pdp()` | `handle_at_command()` |
| **Input** | Nothing (initiates) | AT command string |
| **Output** | Success/fail | "OK" or "ERROR" |
| **State before** | REGISTERED | REGISTERED |
| **State after** | PDP_ACTIVE | PDP_ACTIVE |

**What each command does:**

- `AT+CGDCONT=1,"IP","internet"` — **Define** a PDP context:
  - `1` = context ID (you can have multiple, like multiple network interfaces)
  - `"IP"` = we want IPv4 (could be "IPV6" or "IPV4V6")
  - `"internet"` = the APN (Access Point Name). This is like a hostname that
    tells the carrier which gateway to route your traffic through.
    Real APNs: "fast.t-mobile.com", "hologram", "iot.1nce.net"

  **Networking parallel:** This is configuring a virtual network interface.
  No packets flow yet — you're just telling the network "I want an IP
  connection through this APN."

- `AT+CGACT=1,1` — **Activate** PDP context #1:
  - First `1` = context ID
  - Second `1` = activate (0 would deactivate)

  **Networking parallel:** This is the actual DHCP exchange. The modem
  negotiates with the carrier's GGSN/PGW (gateway) to get an IP address.
  After this succeeds, the device has a routable IP address on the
  cellular network. But the MCU doesn't know the IP yet — that comes
  during PPP IPCP negotiation in Phase 2.

---

### State 4: DATA_MODE / Enter PPP

```
Networking layer:  Layer 2 — Data Link (PPP begins here)
What happens:      The UART switches from human-readable AT commands to
                   binary PPP frames. This is the most important transition
                   in the entire FSM — it's the boundary between "configuring
                   the modem" and "sending actual network traffic."
```

| | Driver sends | Modem receives & responds |
|---|---|---|
| **Step 1** | `"ATD*99#\r"` | Responds `"CONNECT 115200\r\n"` |

| | Driver | Modem |
|---|---|---|
| **Function** | `modem_enter_data_mode()` | `handle_at_command()` |
| **Input** | Nothing (initiates) | `"ATD*99#"` command |
| **Output** | "CONNECT" confirmation | `"CONNECT 115200"` response, then switch to binary |
| **State before** | PDP_ACTIVE | PDP_ACTIVE |
| **State after** | DATA_MODE | DATA_MODE |

**What happens next:**

- `ATD*99#` — Originally a dial command (`ATD` = dial, `*99#` = the PPP
  service code). Tells the modem: "stop accepting AT commands, start
  accepting PPP frames on this UART."

- After `CONNECT`, **every byte on the UART is a PPP/HDLC frame**. No more
  `AT` commands, no more `\r\n`, no more text. The wire now carries binary
  protocol data.

- The driver hands the UART to lwIP's PPP client. The modem enters its
  PPP handler (`handle_ppp_data_mode()`).

**Escaping back:** To return to AT command mode, the driver can send `+++`
(three plus signs with specific timing — 1s silence before and after).
The modem detects this and transitions back to PDP_ACTIVE state.

---

## Phase 2: PPP Data Phase — The PPP Sub-FSM

Once both sides enter DATA_MODE, a **new FSM** takes over within the PPP
protocol. This is where the real networking layers come alive.

### What PPP Frames Look Like on the UART

Every PPP frame is wrapped in HDLC (High-Level Data Link Control):

```
┌──────┬──────────┬──────────┬──────────┬─────────────────┬───────┬──────┐
│ Flag │ Address  │ Control  │ Protocol │    Payload      │  FCS  │ Flag │
│ 0x7E │   0xFF   │   0x03   │  2 bytes │  variable len   │2 bytes│ 0x7E │
└──────┴──────────┴──────────┴──────────┴─────────────────┴───────┴──────┘
```

- `0x7E` — Frame delimiter. Every frame starts and ends with this byte.
- `0xFF` — Broadcast address (always 0xFF in PPP — it's point-to-point).
- `0x03` — Unnumbered Information frame (always 0x03 in PPP).
- Protocol field — tells you what's inside:
  - `0xC021` = LCP (Link Control Protocol)
  - `0x8021` = IPCP (IP Control Protocol)
  - `0x0021` = IPv4 packet
- FCS — Frame Check Sequence (CRC-16). Ensures data integrity.

**Byte stuffing:** If `0x7E` or `0x7D` appears in the payload, it's escaped:
`0x7E` becomes `0x7D 0x5E`, and `0x7D` becomes `0x7D 0x5D`.

### PPP Sub-FSM: State Transitions

```
         DATA_MODE entry
              │
              ▼
    ┌───────────────────┐
    │   LCP_NEGOTIATE   │◄──── Layer 2: Link setup
    └────────┬──────────┘
             │  (LCP Config-Req / Config-Ack exchange)
             ▼
    ┌───────────────────┐
    │   LCP_OPENED      │◄──── Layer 2: Link is up
    └────────┬──────────┘
             │  (Optional: PAP/CHAP authentication)
             ▼
    ┌───────────────────┐
    │  IPCP_NEGOTIATE   │◄──── Layer 3: IP address negotiation
    └────────┬──────────┘
             │  (IPCP Config-Req / Config-Nak / Config-Ack)
             ▼
    ┌───────────────────┐
    │  IPCP_OPENED      │◄──── Layer 3: IP is up, device has an address
    └────────┬──────────┘
             │
             ▼
    ┌───────────────────┐
    │   IP_FORWARDING   │◄──── Layers 3-7: Real packets flow
    └───────────────────┘
```

### PPP Step 1: LCP Negotiation (Layer 2 — Data Link)

```
Networking layer:  Layer 2 — Data Link (configuring the link itself)
What happens:      Both sides agree on link parameters: maximum packet size
                   (MRU), authentication method, whether to compress headers.
Protocol ID:       0xC021
```

| Step | UART direction | Frame content |
|---|---|---|
| 1 | Driver → Modem | LCP Config-Request: "I want MRU=1500, no auth" |
| 2 | Modem → Driver | LCP Config-Ack: "I accept your parameters" |
| 3 | Modem → Driver | LCP Config-Request: "I want MRU=1500" |
| 4 | Driver → Modem | LCP Config-Ack: "I accept your parameters" |

**What to implement (sim_modem side):**

```
Input:   Raw bytes from UART — look for 0x7E frame delimiters
         Parse HDLC: strip flag, check protocol field = 0xC021
         Parse LCP: code=1 (Config-Request), id, options

Output:  LCP Config-Ack: same options echoed back, code=2
         Then send our own LCP Config-Request: code=1, our options

Trigger: Receiving a valid LCP Config-Request
Result:  Both sides have agreed on link parameters. Link is "opened."
```

**Networking lesson:** Every Layer 2 protocol has a negotiation phase. Ethernet
uses auto-negotiation for speed/duplex. WiFi uses association. PPP uses LCP.
The point is always the same: agree on the rules before sending data.

---

### PPP Step 2: IPCP Negotiation (Layer 3 — Network)

```
Networking layer:  Layer 3 — Network (IP address assignment)
What happens:      The driver requests an IP address. The modem (acting as
                   the network) assigns one. Also negotiates DNS servers.
                   This is the PPP equivalent of DHCP.
Protocol ID:       0x8021
```

| Step | UART direction | Frame content |
|---|---|---|
| 1 | Driver → Modem | IPCP Config-Request: "I want IP 0.0.0.0" (= please assign me one) |
| 2 | Modem → Driver | IPCP Config-Nak: "No, use 10.0.0.2 instead" |
| 3 | Driver → Modem | IPCP Config-Request: "I want IP 10.0.0.2" (accepted the suggestion) |
| 4 | Modem → Driver | IPCP Config-Ack: "Confirmed, you are 10.0.0.2" |
| 5 | Modem → Driver | IPCP Config-Request: "My IP is 10.0.0.1, DNS is 10.0.0.1" |
| 6 | Driver → Modem | IPCP Config-Ack: "Confirmed" |

**What to implement (sim_modem side):**

```
Input:   PPP frame with protocol 0x8021
         Parse IPCP: code=1 (Config-Request), look for option 0x03 (IP address)

Logic:   If requested IP is 0.0.0.0:
           Send Config-Nak (code=3) with IP 10.0.0.2 (the IP we're assigning)
         If requested IP is 10.0.0.2:
           Send Config-Ack (code=2) — accept it
         Also send our own Config-Request with our IP (10.0.0.1) and DNS

Output:  IPCP frames back through UART

Trigger: Receiving IPCP Config-Request
Result:  Driver has IP 10.0.0.2, modem has IP 10.0.0.1
         DNS server is 10.0.0.1 (modem will answer DNS queries itself)
```

**Networking lesson:** At Layer 3, every device needs a unique address. On
Ethernet you get one from DHCP. On cellular PPP, you get one from IPCP. The
mechanism differs, but the concept is identical: "Before I can send packets,
I need to know who I am on this network."

After IPCP completes, the driver side's lwIP creates a network interface with
IP 10.0.0.2. From this point, standard socket APIs (`connect`, `send`, `recv`)
work — lwIP wraps everything in IP headers, PPP frames, and HDLC, and sends it
over the UART automatically.

---

### PPP Step 3: IP Packet Forwarding (Layers 3-7)

```
Networking layer:  Layer 3 (IP) → Layer 4 (TCP/UDP) → Layer 7 (App)
What happens:      Real IP packets flow over the PPP link. The driver sends
                   DNS queries, TCP connections, HTTP requests. The sim modem
                   must parse these packets and generate fake responses.
Protocol ID:       0x0021 (IPv4 payload inside PPP)
```

Once IPCP is open, the UART carries PPP frames containing raw IPv4 packets.
The sim_modem must unpack each packet and respond appropriately:

#### DNS Query (UDP, Layer 7)

```
Driver → Modem:
  PPP frame (0x0021) containing:
    IPv4 header: src=10.0.0.2, dst=10.0.0.1, proto=UDP
      UDP header: src_port=random, dst_port=53
        DNS query: "api.example.com" type=A

Modem → Driver:
  PPP frame (0x0021) containing:
    IPv4 header: src=10.0.0.1, dst=10.0.0.2, proto=UDP
      UDP header: src_port=53, dst_port=random
        DNS response: "api.example.com" → 93.184.216.34
```

**What to implement:** Parse UDP packets to port 53. Extract the domain name
from the DNS query. Return a canned IP address. You need to construct a valid
DNS response with correct transaction ID, flags, and answer section.

#### TCP Connection (Layer 4)

```
Driver → Modem:  SYN         (seq=1000, ack=0)
Modem → Driver:  SYN-ACK     (seq=5000, ack=1001)
Driver → Modem:  ACK         (seq=1001, ack=5001)
                 ── connection established ──
Driver → Modem:  PSH,ACK     (HTTP GET request payload)
Modem → Driver:  PSH,ACK     (HTTP 200 response payload)
Driver → Modem:  ACK
Driver → Modem:  FIN,ACK
Modem → Driver:  FIN,ACK
Driver → Modem:  ACK
                 ── connection closed ──
```

**What to implement:** Maintain a small connection table. For each TCP
connection, track: state (SYN_RECEIVED, ESTABLISHED, FIN_WAIT), sequence
numbers, and acknowledgment numbers. Generate correct SYN-ACK, manage the
window, serve a canned payload when you receive an HTTP GET.

**Networking lesson:** TCP's 3-way handshake (SYN → SYN-ACK → ACK) exists
because both sides need to agree on initial sequence numbers. Without this,
neither side knows where in the byte stream the other one is. The sequence
numbers make TCP reliable — if a packet is lost, the receiver notices a gap
and asks for retransmission.

#### ICMP Ping (Layer 3)

```
Driver → Modem:  ICMP Echo Request  (type=8, code=0, id=1, seq=1)
Modem → Driver:  ICMP Echo Reply    (type=0, code=0, id=1, seq=1)
```

**What to implement:** The simplest IP-level response. When you receive an
ICMP packet with type=8 (echo request), swap the source/destination IPs,
change type to 0 (echo reply), recompute the checksum, and send it back.
This is the first thing to implement — if ping works, your PPP + IP layers
are correct.

---

## Complete State Transition Table

This table covers every state transition in both FSMs. Use it as a checklist
when implementing.

### Driver FSM (modem_driver.c)

| # | From State | To State | Trigger | Function | What crosses the UART | Layer |
|---|---|---|---|---|---|---|
| 1 | — | IDLE | `modem_driver_init()` called | `modem_driver_init` | Nothing (UART setup) | L1 |
| 2 | IDLE | SIM_OK | AT+CPIN returns READY | `modem_check_sim` | AT, ATE0, AT+CPIN? → OK, OK, +CPIN: READY | Control |
| 3 | IDLE | ERROR | AT gets no response (5 retries) | `modem_check_sim` | AT → (silence) | Control |
| 4 | IDLE | ERROR | AT+CPIN returns not READY | `modem_check_sim` | AT+CPIN? → +CPIN: SIM PIN | Control |
| 5 | SIM_OK | REGISTERED | AT+CREG returns 0,1 | `modem_register_network` | AT+CSQ, AT+CREG?, AT+COPS? → responses | Control |
| 6 | SIM_OK | ERROR | AT+CREG returns 0,3 (denied) | `modem_register_network` | AT+CREG? → +CREG: 0,3 | Control |
| 7 | REGISTERED | PDP_ACTIVE | AT+CGACT returns OK | `modem_activate_pdp` | AT+CGDCONT, AT+CGACT → OK, OK | L3 |
| 8 | PDP_ACTIVE | DATA_MODE | ATD*99# returns CONNECT | `modem_enter_data_mode` | ATD*99# → CONNECT 115200 | L2 |
| 9 | DATA_MODE | (lwIP) | PPP negotiation completes | (Phase 2) | LCP + IPCP binary frames | L2-L3 |

### Modem FSM (sim_modem.c)

| # | From State | To State | Trigger | Handler | Response sent | Layer |
|---|---|---|---|---|---|---|
| 1 | OFF | READY | Boot delay expires | `sim_modem_task` | Nothing (internal transition) | L1 |
| 2 | READY | SIM_READY | Receives AT+CPIN? | `handle_at_command` | +CPIN: READY\r\nOK | Control |
| 3 | SIM_READY | REGISTERED | Receives AT+CREG? | `handle_at_command` | +CREG: 0,1\r\nOK | Control |
| 4 | REGISTERED | PDP_ACTIVE | Receives AT+CGACT=1,1 | `handle_at_command` | OK | L3 |
| 5 | PDP_ACTIVE | DATA_MODE | Receives ATD*99# | `handle_at_command` | CONNECT 115200 | L2 |
| 6 | DATA_MODE | (PPP) | Receives LCP Config-Req | `handle_ppp_data_mode` | LCP Config-Ack + Config-Req | L2 |
| 7 | DATA_MODE | (PPP) | Receives IPCP Config-Req | `handle_ppp_data_mode` | IPCP Config-Nak/Ack | L3 |
| 8 | DATA_MODE | (PPP) | Receives IPv4 packet | `handle_ppp_data_mode` | ICMP reply / TCP response | L3-L7 |

---

## Mapping Functions to Layers

```
                         ┌─────────────────────────────────────────┐
                         │           YOUR APPLICATION              │
  Layer 7 (App)          │  HTTP GET, MQTT publish, etc.           │
                         │  (runs after PPP link is up)            │
                         └──────────────────┬──────────────────────┘
                                            │ socket API
                         ┌──────────────────┴──────────────────────┐
  Layer 4 (Transport)    │  lwIP: TCP / UDP                        │
                         │  (automatic — you call connect/send)    │
                         └──────────────────┬──────────────────────┘
                                            │ IP packets
                         ┌──────────────────┴──────────────────────┐
  Layer 3 (Network)      │  lwIP: IPv4                             │
                         │  modem_activate_pdp() sets this up      │
                         │  IPCP assigns the IP address             │
                         └──────────────────┬──────────────────────┘
                                            │ PPP frames
                         ┌──────────────────┴──────────────────────┐
  Layer 2 (Data Link)    │  lwIP: PPP/HDLC framing                 │
                         │  modem_enter_data_mode() triggers this  │
                         │  LCP negotiates link params              │
                         └──────────────────┬──────────────────────┘
                                            │ raw bytes
                         ┌──────────────────┴──────────────────────┐
  Layer 1 (Physical)     │  ESP32-S3 UART hardware                 │
                         │  modem_driver_init() / sim_modem_init() │
                         │  GPIO pins, baud rate, electrical       │
                         └──────────────────┬──────────────────────┘
                                            │ wires
                         ┌──────────────────┴──────────────────────┐
  Control Plane          │  AT commands (pre-PPP)                  │
  (out-of-band)          │  modem_check_sim()                      │
                         │  modem_register_network()               │
                         │  modem_activate_pdp()                   │
                         │  modem_enter_data_mode()                │
                         └─────────────────────────────────────────┘
```

---

## Implementation Order (Suggested)

Work through these in order. Each step builds on the previous one, and each
step gives you a testable checkpoint.

| Step | What to implement | How to verify it works |
|---|---|---|
| 1 | `sim_modem_init()` + `modem_driver_init()` — UART setup | Console prints config. No crashes. |
| 2 | `send_response()` + `read_line()` in sim_modem.c | Modem can read a line and echo it back |
| 3 | `send_command_raw()` + `read_response()` in modem_driver.c | Driver can send "AT" and receive bytes |
| 4 | `handle_at_command()` — just "AT" → "OK" | Console shows: TX "AT", RX "OK" |
| 5 | `modem_check_sim()` + remaining AT commands | Console shows: "SIM OK" |
| 6 | `modem_register_network()` | Console shows: "Registered, RSSI=20, Op=Fake Cellular" |
| 7 | `modem_activate_pdp()` | Console shows: "PDP active" |
| 8 | `modem_enter_data_mode()` | Console shows: "CONNECT — entered data mode" |
| 9 | PPP LCP negotiation | Both sides log "LCP opened" |
| 10 | PPP IPCP negotiation | Driver logs "Got IP: 10.0.0.2" |
| 11 | ICMP ping response | Driver pings 10.0.0.1, gets reply |
| 12 | DNS response | Driver resolves "example.com" → canned IP |
| 13 | TCP 3-way handshake | Driver connects to canned IP:80, handshake completes |
| 14 | HTTP response | Driver sends GET, receives canned HTML |

After step 8, you've completed Phase 1. Steps 9-14 are Phase 2 (PPP).
Each step teaches you a new networking layer by forcing you to implement
the protocol that runs at that layer.
