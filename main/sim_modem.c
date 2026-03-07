#include "sim_modem.h"
#include "board_config.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "SIM_MODEM";

// MODEM_SIM_UART_NUM, MODEM_SIM_TX_PIN, MODEM_SIM_RX_PIN,
// MODEM_SIM_RTS_PIN, MODEM_SIM_CTS_PIN are set in board_config.h
// — see that file for pin assignments and wiring.
#define UART_BAUD           115200
#define FLOW_CONTROL_ENABLED  0

static sim_modem_config_t s_sim_modem_cfg = {
    .uart_num      = MODEM_SIM_UART_NUM,
    .tx_pin        = MODEM_SIM_TX_PIN,
    .rx_pin        = MODEM_SIM_RX_PIN,
    .rts_pin       = FLOW_CONTROL_ENABLED ? MODEM_SIM_RTS_PIN : -1,
    .cts_pin       = FLOW_CONTROL_ENABLED ? MODEM_SIM_CTS_PIN : -1,
    .baud_rate     = UART_BAUD,
    .flow_control  = FLOW_CONTROL_ENABLED,
    .state         = SIM_MODEM_STATE_OFF,
};

// Real modems default to echo ON (ATE1). Every character the driver sends is
// echoed back before the modem's actual response. ATE0 disables this.
static int echo_enabled = 1;

// ============================================================================
//  INTERNAL HELPERS
// ============================================================================

// ---------------------------------------------------------------------------
//  send_response
//
//  Sends a string back to the driver over the modem's UART TX.
//  All modem responses end with \r\n.
//
//    1. Use uart_write_bytes() to send the response string
//    2. Append \r\n after the response
//    3. (Optional) Add a small delay to simulate real modem response latency
// ---------------------------------------------------------------------------
static void send_response(sim_modem_config_t *config, const char *response)
{
    // What goes on the wire: "\r\n" + response text + "\r\n"
    // Real modems prefix responses with \r\n so the driver can distinguish
    // modem output from echoed commands.
    uart_write_bytes(config->uart_num, "\r\n", 2);
    uart_write_bytes(config->uart_num, response, strlen(response));
    uart_write_bytes(config->uart_num, "\r\n", 2);
}

// ---------------------------------------------------------------------------
//  read_line
//
//  Reads bytes from UART RX until a line terminator ('\r' or '\n') is found.
//  Returns the number of bytes read (excluding the terminator).
//
//    1. Read one byte at a time from UART using uart_read_bytes() with a timeout
//    2. Accumulate into the provided buffer
//    3. Stop when '\r' or '\n' is received, or buffer is full
//    4. Null-terminate the buffer
//    5. Return the length, or -1 on timeout
// ---------------------------------------------------------------------------
static int read_line(sim_modem_config_t *config, char *buf, size_t buf_size,
                     TickType_t timeout)
{
    // Read one byte at a time, accumulating into buf until we hit a line
    // terminator ('\r' or '\n') or run out of space/time.
    // AT commands are always single-line, terminated by '\r' from the driver.
    size_t pos = 0;

    while (pos < buf_size - 1) {
        unsigned char c;
        // Block on each byte up to the caller's timeout.
        int got = uart_read_bytes(config->uart_num, &c, 1, timeout);

        // No byte arrived within timeout — return what we have (or -1 if empty).
        if (got <= 0) {
            return (pos > 0) ? (int)pos : -1;
        }

        // '\r' or '\n' marks end of command — don't store the terminator.
        if (c == '\r' || c == '\n') {
            if (pos == 0) {
                // Leading blank line (e.g. the '\n' after a '\r') — skip it.
                continue;
            }
            break;
        }

        // Accumulate printable command bytes.
        buf[pos++] = (char)c;
    }

    // Null-terminate so strcmp/strncmp work in handle_at_command.
    buf[pos] = '\0';
    return (int)pos;
}

// ---------------------------------------------------------------------------
//  handle_at_command
//
//  The AT command dispatcher. Receives a null-terminated command string,
//  checks it against known commands, and sends back the appropriate response.
//
//  The modem's state machine gates which commands are valid.
//
//  Supported commands and their behavior:
//
//  ┌─────────────────┬────────────────────────────────────────────────┐
//  │ Command         │ Expected behavior                              │
//  ├─────────────────┼────────────────────────────────────────────────┤
//  │ "AT"            │ Reply "OK". Simplest ping — always works.      │
//  │ "ATE0"          │ Reply "OK". Disable echo (echo_enabled = 0). │
//  │ "ATE1"          │ Reply "OK". Re-enable echo (echo_enabled = 1).│
//  │ "AT+CPIN?"      │ If state >= READY:                             │
//  │                 │   Set state = SIM_READY                        │
//  │                 │   Reply "+CPIN: READY\r\nOK"                   │
//  │ "AT+CSQ"        │ Reply "+CSQ: 20,0\r\nOK"  (good signal)        │
//  │ "AT+CREG?"      │ If state >= SIM_READY:                         │
//  │                 │   Set state = REGISTERED                       │
//  │                 │   Reply "+CREG: 0,1\r\nOK"                     │
//  │ "AT+CEREG?"     │ Same as CREG — reply "+CEREG: 0,1\r\nOK"       │
//  │ "AT+COPS?"      │ Reply "+COPS: 0,0,\"Fake Cellular\"\r\nOK"     │
//  │ "AT+CGDCONT=…"  │ Reply "OK". (Store APN if you want to.)        │
//  │ "AT+CGACT=1,1"  │ If state >= REGISTERED:                        │
//  │                 │   Set state = PDP_ACTIVE                       │
//  │                 │   Reply "OK"                                   │
//  │ "ATD*99#"       │ If state == PDP_ACTIVE:                        │
//  │                 │   Reply "CONNECT 115200"                       │
//  │                 │   Set state = DATA_MODE                        │
//  │                 │   (Caller should then switch to PPP handler)   │
//  │ (unknown)       │ Reply "ERROR"                                  │
//  └─────────────────┴────────────────────────────────────────────────┘
//
//  Hint: use strncmp() or strstr() for matching. AT commands are
//  case-insensitive on real modems, but case-sensitive is fine here.
//
//  Returns: the new state after processing the command
// ---------------------------------------------------------------------------
static sim_modem_state_t handle_at_command(sim_modem_config_t *config,
                                           const char *cmd)
{
    ESP_LOGI(TAG, "RX cmd: \"%s\" (state=%d)", cmd, config->state);

    // Echo the command back before responding, just like real modem hardware.
    // The driver sees its own command repeated, then the modem's response.
    if (echo_enabled) {
        uart_write_bytes(config->uart_num, cmd, strlen(cmd));
        uart_write_bytes(config->uart_num, "\r\n", 2);
    }

    // Ping — always valid, proves the modem is alive.
    if (strcmp(cmd, "AT") == 0) {
        send_response(config, "OK");

    // ATE0 — disable echo. Driver won't see its commands repeated anymore.
    } else if (strcmp(cmd, "ATE0") == 0) {
        echo_enabled = 0;
        send_response(config, "OK");

    // ATE1 — re-enable echo (back to default behavior).
    } else if (strcmp(cmd, "ATE1") == 0) {
        echo_enabled = 1;
        send_response(config, "OK");

    // SIM status — transitions modem from READY → SIM_READY.
    } else if (strcmp(cmd, "AT+CPIN?") == 0) {
        if (config->state >= SIM_MODEM_STATE_READY) {
            config->state = SIM_MODEM_STATE_SIM_READY;
            send_response(config, "+CPIN: READY\r\nOK");
        } else {
            send_response(config, "ERROR");
        }

    // Signal quality — always report good signal (RSSI=20, BER=0).
    } else if (strcmp(cmd, "AT+CSQ") == 0) {
        send_response(config, "+CSQ: 20,0\r\nOK");

    // Network registration — transitions SIM_READY → REGISTERED.
    } else if (strcmp(cmd, "AT+CREG?") == 0) {
        if (config->state >= SIM_MODEM_STATE_SIM_READY) {
            config->state = SIM_MODEM_STATE_REGISTERED;
            send_response(config, "+CREG: 0,1\r\nOK"); // Not sending unsolicited updates, registered
        } else {
            send_response(config, "+CREG: 0,0\r\nOK"); // Not sending unsolicited updates, unregistered
        }

    // EPS registration — same behavior as CREG for our purposes.
    } else if (strcmp(cmd, "AT+CEREG?") == 0) {
        if (config->state >= SIM_MODEM_STATE_SIM_READY) {
            config->state = SIM_MODEM_STATE_REGISTERED;
            send_response(config, "+CEREG: 0,1\r\nOK");
        } else {
            send_response(config, "+CEREG: 0,0\r\nOK");
        }

    // Operator query — report fake operator name.
    } else if (strcmp(cmd, "AT+COPS?") == 0) {
        send_response(config, "+COPS: 0,0,\"Fake Cellular\"\r\nOK");

    // Define PDP context — accept any APN, just acknowledge.
    } else if (strncmp(cmd, "AT+CGDCONT=", 11) == 0) {
        send_response(config, "OK");

    // Activate PDP context — transitions REGISTERED → PDP_ACTIVE.
    } else if (strcmp(cmd, "AT+CGACT=1,1") == 0) {
        if (config->state >= SIM_MODEM_STATE_REGISTERED) {
            config->state = SIM_MODEM_STATE_PDP_ACTIVE;
            send_response(config, "OK");
        } else {
            send_response(config, "ERROR");
        }

    // Dial PPP — transitions PDP_ACTIVE → DATA_MODE.
    // After CONNECT, UART switches from AT text to binary PPP frames.
    } else if (strcmp(cmd, "ATD*99#") == 0) {
        if (config->state == SIM_MODEM_STATE_PDP_ACTIVE) {
            send_response(config, "CONNECT 115200");
            config->state = SIM_MODEM_STATE_DATA_MODE;
        } else {
            send_response(config, "NO CARRIER");
        }

    // Unknown command — real modems respond with ERROR.
    } else {
        ESP_LOGW(TAG, "Unknown AT command: \"%s\"", cmd);
        send_response(config, "ERROR");
    }

    return config->state;
}

// ============================================================================
//  PPP HELPERS & HANDLERS
//
//  Phase 2 flow after ATD*99# enters DATA_MODE (FSM S5):
//    S5.0: Byte read loop + unstuffing  — handle_ppp_data_mode()
//    S5.1: Frame delimiter + accumulate — handle_ppp_data_mode()
//    S5.2: Frame dispatch by protocol   — handle_ppp_data_mode()
//    S5.3: LCP negotiation              — handle_lcp_frame()
//    S5.4: IPCP negotiation             — handle_ipcp_frame()
//    S5.5: IP packet routing            — handle_ipv4_packet()
//    S5.6: +++ escape detection         — handle_ppp_data_mode()
// ============================================================================

// Simulated IP addresses assigned during IPCP
#define SIM_MODEM_IP    {10, 0, 0, 1}
#define SIM_PEER_IP     {10, 0, 0, 2}
#define SIM_DNS_IP      {10, 0, 0, 1}

// ---------------------------------------------------------------------------
//  PPP Async HDLC framing (RFC 1662) — real modem behavior
//
//  Real cellular modems (SIM7600, BG96, etc.) speak standard PPP over async
//  serial. The MCU (lwIP) and modem exchange PPP frames using:
//
//  1. HDLC-like framing: frames are delimited by 0x7E (FLAG). Consecutive
//     flags are legal fill between frames.
//
//  2. Byte stuffing (escaping): To avoid 0x7E and 0x7D in the payload being
//     mistaken for flags or escape sequences, RFC 1662 defines:
//       - 0x7E in payload → send 0x7D 0x5E (escape + 0x7E^0x20)
//       - 0x7D in payload → send 0x7D 0x5D (escape + 0x7D^0x20)
//       - Other chars in ACCM → 0x7D (char^0x20)
//     The modem RECEIVES stuffed bytes, UNSTUFFS before parsing, STUFFS before
//     sending. We mirror that.
//
//  3. Frame format on wire:
//     [0x7E] [stuffed payload: addr 0xFF, ctrl 0x03, protocol, data] [FCS 2B] [0x7E]
//     We omit FCS for simplicity (sim); real modems verify it.
//
//  4. Data flow (mirrors real modem):
//     MCU sends: 0x7E [0x7D 0x5F 0x7D 0x23 0xC0 0x21 ...] 0x7E  (stuffed)
//     Modem receives, unstuffs: 0xFF 0x03 0xC021 ... (logical frame)
//     Modem sends: 0x7E [stuffed response] 0x7E
//     MCU receives, unstuffs (lwIP does this).
//
//  This sim acts as the modem: we unstuff on RX, stuff on TX.
// ---------------------------------------------------------------------------

#define PPP_FLAG_BYTE    0x7E
#define PPP_ESCAPE_BYTE  0x7D
#define PPP_TRANS_BYTE   0x20

// ---------------------------------------------------------------------------
//  PPP FCS (Frame Check Sequence)
//  RFC 1662 standard CRC-16 for PPP frames (computed bitwise to save space)
// ---------------------------------------------------------------------------
static unsigned short ppp_fcs16(unsigned short fcs, const unsigned char *cp, size_t len)
{
    while (len--) {
        fcs ^= *cp++;
        for (int i = 0; i < 8; i++) {
            if (fcs & 1)
                fcs = (fcs >> 1) ^ 0x8408;
            else
                fcs >>= 1;
        }
    }
    return fcs;
}

// ---------------------------------------------------------------------------
//  send_ppp_frame
//
//  Wraps payload in 0x7E delimiters, applies byte stuffing (RFC 1662), and
//  writes to UART. Real modems stuff 0x7E and 0x7D (and ACCM chars) before
//  transmit so the peer can parse frames correctly.
// ---------------------------------------------------------------------------
static void send_ppp_frame(sim_modem_config_t *config,
                           const unsigned char *frame, size_t len,
                           unsigned char ppp_flag)
{
    unsigned char tx_buf[1024];
    size_t tx_len = 0;

    // Calculate FCS (PPP checksum)
    unsigned short fcs = 0xffff;
    fcs = ppp_fcs16(fcs, frame, len);
    fcs ^= 0xffff;

    uart_write_bytes(config->uart_num, (const char *)&ppp_flag, 1);  // opening 0x7E

    // Write the payload
    for (size_t i = 0; i < len; i++) {
        // If buffer is almost full (need space for up to 2 stuffed bytes), flush it
        if (tx_len >= sizeof(tx_buf) - 2) {
            uart_write_bytes(config->uart_num, (const char *)tx_buf, tx_len);
            tx_len = 0;
        }

        unsigned char c = frame[i];
        if (c == PPP_FLAG_BYTE || c == PPP_ESCAPE_BYTE || c < 0x20) {
            tx_buf[tx_len++] = PPP_ESCAPE_BYTE;
            tx_buf[tx_len++] = c ^ PPP_TRANS_BYTE;
        } else {
            tx_buf[tx_len++] = c;
        }
    }
    
    // Write the FCS bytes (LSB first)
    unsigned char fcs_bytes[2] = { fcs & 0x00FF, (fcs >> 8) & 0x00FF };
    for (size_t i = 0; i < 2; i++) {
        if (tx_len >= sizeof(tx_buf) - 2) {
            uart_write_bytes(config->uart_num, (const char *)tx_buf, tx_len);
            tx_len = 0;
        }

        unsigned char c = fcs_bytes[i];
        if (c == PPP_FLAG_BYTE || c == PPP_ESCAPE_BYTE || c < 0x20) {
            tx_buf[tx_len++] = PPP_ESCAPE_BYTE;
            tx_buf[tx_len++] = c ^ PPP_TRANS_BYTE;
        } else {
            tx_buf[tx_len++] = c;
        }
    }

    if (tx_len > 0) {
        uart_write_bytes(config->uart_num, (const char *)tx_buf, tx_len);
    }
    uart_write_bytes(config->uart_num, (const char *)&ppp_flag, 1);  // closing 0x7E
}

// ---------------------------------------------------------------------------
//  Internet checksum (RFC 1071) — used for IP, ICMP, TCP, UDP headers.
// ---------------------------------------------------------------------------
static unsigned short ip_checksum(const unsigned char *data, size_t len)
{
    unsigned long sum = 0;
    while (len > 1) {
        sum += ((unsigned short)data[0] << 8) | data[1];
        data += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += (unsigned short)data[0] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (unsigned short)(~sum);
}

// ---------------------------------------------------------------------------
//  handle_lcp_frame  (FSM S5.3)
//  After ATD*99# enters DATA_MODE, LCP is the first PPP control protocol.
//  Must complete before IPCP can assign IP addresses.
//
//  Vocab:
//    - Config-Request: the driver proposing its desired link parameters
//      to the modem and asking the modem to accept or reject them.
//    - Config-Ack: the modem's response saying "I accept all your
//      proposed parameters." Built by echoing the request back with
//      code changed from 0x01 to 0x02.
//    - Link parameters: settings that govern the PPP link itself, e.g.
//      MRU (max receive unit / max packet size), authentication method
//      (PAP/CHAP), and protocol compression. Negotiated before any IP
//      traffic can flow.
// ---------------------------------------------------------------------------
static void handle_lcp_frame(sim_modem_config_t *config,
                             const unsigned char *frame_buf,
                             size_t frame_len,
                             unsigned char ppp_flag)
{
    ESP_LOGI(TAG, "PPP frame complete: LCP (len=%u)", (unsigned int)frame_len);

    // S5.3: LCP negotiation (minimal implementation).
    // Parse LCP Config-Request from the driver and send:
    //   1) LCP Config-Ack (echo options back with code=2)
    //   2) Our own LCP Config-Request (MRU=1500)
    if (frame_len >= 8) {
        const unsigned char *lcp = &frame_buf[4];
        unsigned char lcp_code = lcp[0];
        unsigned char lcp_id = lcp[1];
        unsigned short lcp_len = ((unsigned short)lcp[2] << 8) | lcp[3];

        // If: Is this a Config-Request (0x01), with a valid length, that fits in the frame?
        if (lcp_code == 0x01 && lcp_len >= 4 && (4U + lcp_len) <= frame_len) {
            // Condition met:
            // The driver sent us its desired PPP link settings.
            // We accept them (send Ack) and then reply (send our own Config-Request).
            unsigned char ack_frame[512];
            size_t ack_len = 4U + lcp_len;

            // Send data:
            // Build LCP Config-Ack by echoing requester options.
            // [0:1, 0xFF addr] [1:2, 0x03 ctrl] [2:4, 0xC021 LCP proto] [4:4+lcp_len, driver's options echoed back with code=0x02]
            ack_frame[0] = 0xFF;
            ack_frame[1] = 0x03;
            ack_frame[2] = 0xC0;
            ack_frame[3] = 0x21;
            memcpy(&ack_frame[4], lcp, lcp_len);
            ack_frame[4] = 0x02; // Config-Ack

            send_ppp_frame(config, ack_frame, ack_len, ppp_flag);

            ESP_LOGI(TAG, "LCP Config-Ack sent (id=%u, len=%u)",
                     (unsigned int)lcp_id, (unsigned int)lcp_len);

            // Send data:
            // Send our own LCP Config-Request (MRU option = 1500 / 0x05DC).
            // [0:4, 0xFF/0x03/0xC021 PPP hdr] [4:5, 0x01 Config-Request] [5:6, id] [6:8, length=8] [8:12, MRU option: type=0x01 len=4 val=0x05DC]
            static unsigned char local_lcp_id = 0x40;
            unsigned char req_frame[] = {
                0xFF, 0x03, 0xC0, 0x21,
                0x01, local_lcp_id++, 0x00, 0x08,
                0x01, 0x04, 0x05, 0xDC
            };

            send_ppp_frame(config, req_frame, sizeof(req_frame), ppp_flag);

            ESP_LOGI(TAG, "LCP Config-Request sent (id=%u, MRU=1500)",
                     (unsigned int)req_frame[5]);
        } else if (lcp_code == 0x05 && lcp_len >= 4 && (4U + lcp_len) <= frame_len) {
            // Handle Terminate-Request (0x05) by sending Terminate-Ack (0x06)
            unsigned char ack_frame[512];
            size_t ack_len = 4U + lcp_len;
            
            ack_frame[0] = 0xFF;
            ack_frame[1] = 0x03;
            ack_frame[2] = 0xC0;
            ack_frame[3] = 0x21;
            memcpy(&ack_frame[4], lcp, lcp_len);
            ack_frame[4] = 0x06; // Terminate-Ack
            
            send_ppp_frame(config, ack_frame, ack_len, ppp_flag);
            ESP_LOGI(TAG, "LCP Terminate-Ack sent (id=%u)", (unsigned int)lcp_id);
            
        } else {
            ESP_LOGW(TAG, "LCP frame received but unhandled code: 0x%02X", lcp_code);
        }
    } else {
        ESP_LOGW(TAG, "LCP frame too short for negotiation");
    }
}

// ---------------------------------------------------------------------------
//  handle_ipcp_frame  (FSM S5.4)
//  After LCP opens, IPCP assigns IP addresses. This is the PPP equivalent
//  of DHCP — the modem tells the driver what IP to use.
//
//  Vocab:
//    - IPCP: Internet Protocol Control Protocol. Runs inside PPP (proto
//      0x8021) to negotiate Layer 3 parameters before IP traffic flows.
//    - Config-Nak: "I reject the value you proposed, use this instead."
//      Sent when the driver requests IP 0.0.0.0 (meaning "assign me one").
//      The modem Naks with the IP it wants the driver to use (10.0.0.2).
//    - Config-Ack: "I accept your proposed value." Sent when the driver
//      re-requests with the correct IP after being Nak'd.
//    - IP-Address option (type 0x03): the IPCP option that carries the
//      proposed IP address. 6 bytes: type(1) + len(1) + IPv4 addr(4).
//    - Primary/Secondary DNS (types 0x81/0x83): IPCP options for DNS
//      server addresses. The modem proposes these in its own Config-Request.
// ---------------------------------------------------------------------------
static void handle_ipcp_frame(sim_modem_config_t *config,
                              const unsigned char *frame_buf,
                              size_t frame_len,
                              unsigned char ppp_flag)
{
    ESP_LOGI(TAG, "PPP frame complete: IPCP (len=%u)", (unsigned int)frame_len);

    if (frame_len < 8) {
        ESP_LOGW(TAG, "IPCP frame too short");
        return;
    }

    const unsigned char *ipcp = &frame_buf[4];
    unsigned char code = ipcp[0];
    unsigned char id   = ipcp[1];
    unsigned short ipcp_len = ((unsigned short)ipcp[2] << 8) | ipcp[3];

    // Is this a valid Config-Request (code 0x01) that fits in the frame?
    if (code == 0x01 && ipcp_len >= 4 && (4U + ipcp_len) <= frame_len) {
        // The driver is requesting an IP address from us.
        // Scan its options looking for the IP-Address option (type 0x03, 6 bytes).
        const unsigned char peer_ip[] = SIM_PEER_IP;
        const unsigned char *opt = &ipcp[4];
        size_t opt_remaining = ipcp_len - 4;
        int ip_option_ok = 0;

        // Walk the IPCP options TLV chain. Each option is:
        //   [0:1, type] [1:2, length (includes type+len bytes)] [2:length, value]
        // We're looking for type=0x03 (IP-Address), length=6, value=4-byte IPv4.
        // If the driver proposes 10.0.0.2 (our intended peer IP), we Ack.
        // If it proposes 0.0.0.0 or anything else, we Nak with 10.0.0.2.
        while (opt_remaining >= 2) {
            unsigned char opt_type = opt[0];
            unsigned char opt_len  = opt[1];
            if (opt_len < 2 || opt_len > opt_remaining) break;

            if (opt_type == 0x03 && opt_len == 6) {
                // Found the IP-Address option — does it match the IP we want to assign?
                if (memcmp(&opt[2], peer_ip, 4) == 0) {
                    ip_option_ok = 1;
                }
            }
            opt += opt_len;
            opt_remaining -= opt_len;
        }

        if (ip_option_ok) {
            // Driver requested the IP we want to assign — accept it.
            // [0:4, 0xFF/0x03/0x8021 PPP hdr] [4:4+ipcp_len, driver's options echoed back with code=0x02]
            unsigned char ack[512];
            size_t ack_len = 4U + ipcp_len;
            ack[0] = 0xFF; ack[1] = 0x03;
            ack[2] = 0x80; ack[3] = 0x21;
            memcpy(&ack[4], ipcp, ipcp_len);
            ack[4] = 0x02; // Config-Ack

            send_ppp_frame(config, ack, ack_len, ppp_flag);
            ESP_LOGI(TAG, "IPCP Config-Ack sent (peer IP 10.0.0.2 accepted)");
        } else {
            // Driver requested 0.0.0.0 or wrong IP — Nak with the IP we want to assign.
            // [0:4, PPP hdr] [4:5, 0x03 Config-Nak] [5:6, id] [6:8, len] [8:14, IP-Address option with 10.0.0.2]
            unsigned char nak[] = {
                0xFF, 0x03, 0x80, 0x21,
                0x03, id, 0x00, 0x0A,
                0x03, 0x06, peer_ip[0], peer_ip[1], peer_ip[2], peer_ip[3]
            };

            send_ppp_frame(config, nak, sizeof(nak), ppp_flag);
            ESP_LOGI(TAG, "IPCP Config-Nak sent (suggesting 10.0.0.2)");
        }

        // Send our own IPCP Config-Request: propose modem's IP only.
        // [0:4, PPP hdr] [4:5, 0x01 Config-Req] [5:6, id] [6:8, len=10] [8:14, IP opt 10.0.0.1]
        const unsigned char modem_ip[] = SIM_MODEM_IP;
        static unsigned char local_ipcp_id = 0x60;
        unsigned char req[] = {
            0xFF, 0x03, 0x80, 0x21,
            0x01, local_ipcp_id++, 0x00, 0x0A,
            0x03, 0x06, modem_ip[0], modem_ip[1], modem_ip[2], modem_ip[3]
        };

        send_ppp_frame(config, req, sizeof(req), ppp_flag);
        ESP_LOGI(TAG, "IPCP Config-Request sent (modem=10.0.0.1)");

    } else if (code == 0x02) {
        // IPCP Config-Ack from driver — peer accepted our IP config.
        ESP_LOGI(TAG, "IPCP Config-Ack received — IP layer is up");
    } else {
        ESP_LOGW(TAG, "IPCP frame: unhandled code=0x%02X", code);
    }
}

// ---------------------------------------------------------------------------
//  handle_ipv4_packet  (FSM S5.5)
//  After IPCP opens, real IP packets arrive wrapped in PPP frames.
//  Extract IP header, dispatch by protocol: ICMP, UDP (DNS), TCP.
//
//  Vocab:
//    - IPv4 header: 20+ bytes at start of every IP packet. Contains
//      protocol (byte 9: 1=ICMP, 6=TCP, 17=UDP), src/dst IPs (bytes
//      12-19), total length, checksum, and header length.
//    - ICMP Echo Request/Reply: ping. Type 8 = request, type 0 = reply.
//      We swap src/dst IPs, change type to 0, recompute checksums.
//    - UDP: connectionless datagrams. We check dst port 53 for DNS.
//    - DNS: query/response over UDP port 53. Transaction ID ties a
//      response to its query. We return a canned A record.
//    - TCP: connection-oriented. SYN (flags=0x02) starts 3-way handshake.
//      SYN-ACK (flags=0x12) is the server's reply. FIN (flags=0x01)
//      closes the connection.
// ---------------------------------------------------------------------------
static void handle_ipv4_packet(sim_modem_config_t *config,
                               const unsigned char *frame_buf,
                               size_t frame_len,
                               unsigned char ppp_flag)
{
    // IP packet starts at frame_buf[4] (after PPP addr/ctrl/proto).
    // PPP frame layout: [0:4, 0xFF/0x03/0x0021 PPP hdr] [4:frame_len, IPv4 packet]
    // IPv4 header:       [0:1, ver+hdr_len] [2:4, total_len] [9:10, protocol] [12:16, src IP] [16:20, dst IP]
    const unsigned char *ip = &frame_buf[4];
    size_t ip_len = frame_len - 4;

    if (ip_len < 20) {
        ESP_LOGW(TAG, "IPv4 packet too short (%u bytes)", (unsigned int)ip_len);
        return;
    }

    // Parse the fields we need for dispatching and response construction.
    unsigned char ip_hdr_len = (ip[0] & 0x0F) * 4;
    unsigned short total_len = ((unsigned short)ip[2] << 8) | ip[3];
    unsigned char protocol   = ip[9];

    ESP_LOGI(TAG, "IPv4 packet: proto=%u, len=%u, src=%u.%u.%u.%u, dst=%u.%u.%u.%u",
             protocol, total_len,
             ip[12], ip[13], ip[14], ip[15],
             ip[16], ip[17], ip[18], ip[19]);

    // Is this ICMP (protocol=1) with enough bytes for the ICMP header (8 bytes after IP header)?
    if (protocol == 1 && ip_len >= (size_t)(ip_hdr_len + 8)) {
        // S5.5a: ICMP — respond to echo request (ping).
        // ICMP header: [0:1, type] [1:2, code] [2:4, checksum] [4:8, id+seq]
        const unsigned char *icmp = &ip[ip_hdr_len];
        // Type 8 = Echo Request (ping), Type 0 = Echo Reply (pong).
        if (icmp[0] == 8) {
            // Build ICMP Echo Reply: swap src/dst IP, set type=0, fix checksums.
            unsigned char reply[512];
            size_t reply_ip_len = total_len;
            if (reply_ip_len > sizeof(reply) - 4) return;

            // PPP header for IPv4
            reply[0] = 0xFF; reply[1] = 0x03;
            reply[2] = 0x00; reply[3] = 0x21;

            memcpy(&reply[4], ip, reply_ip_len);
            unsigned char *rip = &reply[4];

            // Swap src and dst IP
            unsigned char tmp[4];
            memcpy(tmp, &rip[12], 4);
            memcpy(&rip[12], &rip[16], 4);
            memcpy(&rip[16], tmp, 4);

            // Zero IP checksum, recompute
            rip[10] = 0; rip[11] = 0;
            unsigned short ip_cksum = ip_checksum(rip, ip_hdr_len);
            rip[10] = (ip_cksum >> 8) & 0xFF;
            rip[11] = ip_cksum & 0xFF;

            // ICMP: set type=0 (echo reply), recompute checksum
            unsigned char *ricmp = &rip[ip_hdr_len];
            ricmp[0] = 0;  // type = Echo Reply
            ricmp[2] = 0; ricmp[3] = 0;  // zero checksum
            size_t icmp_len = reply_ip_len - ip_hdr_len;
            unsigned short icmp_cksum = ip_checksum(ricmp, icmp_len);
            ricmp[2] = (icmp_cksum >> 8) & 0xFF;
            ricmp[3] = icmp_cksum & 0xFF;

            send_ppp_frame(config, reply, 4 + reply_ip_len, ppp_flag);
            ESP_LOGI(TAG, "ICMP Echo Reply sent");
        }

    // Is this UDP (protocol=17) with enough bytes for the UDP header (8 bytes after IP header)?
    } else if (protocol == 17 && ip_len >= (size_t)(ip_hdr_len + 8)) {
        // S5.5b: UDP — check for DNS queries (dst port 53).
        // UDP header: [0:2, src port] [2:4, dst port] [4:6, length] [6:8, checksum]
        // DNS payload starts at udp[8].
        const unsigned char *udp = &ip[ip_hdr_len];
        unsigned short dst_port = ((unsigned short)udp[2] << 8) | udp[3];

        if (dst_port == 53 && ip_len >= (size_t)(ip_hdr_len + 8 + 12)) {
            // DNS query detected — build a canned response returning 10.0.0.1
            // for any A-record query.
            //
            // DNS message layout (inside UDP payload):
            //   [0:2, txn ID] [2:4, flags] [4:6, qdcount] [6:8, ancount]
            //   [8:10, nscount] [10:12, arcount] [12:..., question section]
            //   [..., answer section (we append this)]
            const unsigned char *dns = &udp[8];
            unsigned short txn_id = ((unsigned short)dns[0] << 8) | dns[1];

            // Measure the question section: walk QNAME labels until null terminator,
            // then skip QTYPE(2) + QCLASS(2).
            size_t dns_payload_len = ip_len - ip_hdr_len - 8;
            size_t qname_start = 12;
            size_t pos = qname_start;
            while (pos < dns_payload_len && dns[pos] != 0) {
                pos += 1 + dns[pos];
            }
            pos++;           // skip null terminator
            pos += 4;        // skip QTYPE(2) + QCLASS(2)
            size_t question_len = pos - qname_start;

            // Build response: DNS header + echoed question + answer RR.
            // Answer RR uses a name pointer (0xC00C) back to the question's QNAME.
            //   [0:2, 0xC00C name pointer] [2:4, type=A(1)] [4:6, class=IN(1)]
            //   [6:10, TTL=60s] [10:12, rdlength=4] [12:16, IPv4 addr]
            const unsigned char modem_ip[] = SIM_MODEM_IP;
            unsigned char dns_resp[256];
            size_t dr = 0;

            // DNS header: same txn ID, response flags, 1 question, 1 answer.
            dns_resp[dr++] = (unsigned char)(txn_id >> 8);
            dns_resp[dr++] = (unsigned char)(txn_id & 0xFF);
            dns_resp[dr++] = 0x81; dns_resp[dr++] = 0x80;
            dns_resp[dr++] = 0x00; dns_resp[dr++] = 0x01;
            dns_resp[dr++] = 0x00; dns_resp[dr++] = 0x01;
            dns_resp[dr++] = 0x00; dns_resp[dr++] = 0x00;
            dns_resp[dr++] = 0x00; dns_resp[dr++] = 0x00;

            // Echo the question section verbatim.
            if (qname_start + question_len <= dns_payload_len &&
                dr + question_len + 16 <= sizeof(dns_resp)) {
                memcpy(&dns_resp[dr], &dns[qname_start], question_len);
                dr += question_len;

                // Answer RR: pointer to QNAME + A record with modem's IP.
                dns_resp[dr++] = 0xC0; dns_resp[dr++] = 0x0C;
                dns_resp[dr++] = 0x00; dns_resp[dr++] = 0x01;
                dns_resp[dr++] = 0x00; dns_resp[dr++] = 0x01;
                dns_resp[dr++] = 0x00; dns_resp[dr++] = 0x00;
                dns_resp[dr++] = 0x00; dns_resp[dr++] = 0x3C;
                dns_resp[dr++] = 0x00; dns_resp[dr++] = 0x04;
                dns_resp[dr++] = modem_ip[0]; dns_resp[dr++] = modem_ip[1];
                dns_resp[dr++] = modem_ip[2]; dns_resp[dr++] = modem_ip[3];

                // Wrap DNS response in UDP, IP, and PPP headers.
                // Reuse the incoming packet as a template — swap addresses/ports.
                unsigned char reply[512];
                size_t udp_total = 8 + dr;
                size_t ip_total  = ip_hdr_len + udp_total;
                if (ip_total > sizeof(reply) - 4) return;

                // PPP header
                reply[0] = 0xFF; reply[1] = 0x03;
                reply[2] = 0x00; reply[3] = 0x21;

                // Copy original IP header, swap src/dst.
                memcpy(&reply[4], ip, ip_hdr_len);
                unsigned char *rip = &reply[4];
                unsigned char tmp[4];
                memcpy(tmp, &rip[12], 4);
                memcpy(&rip[12], &rip[16], 4);
                memcpy(&rip[16], tmp, 4);
                rip[2] = (ip_total >> 8) & 0xFF;
                rip[3] = ip_total & 0xFF;
                rip[10] = 0; rip[11] = 0;
                unsigned short ck = ip_checksum(rip, ip_hdr_len);
                rip[10] = (ck >> 8) & 0xFF;
                rip[11] = ck & 0xFF;

                // UDP header: swap ports, set length, zero checksum.
                unsigned char *rudp = &rip[ip_hdr_len];
                rudp[0] = udp[2]; rudp[1] = udp[3];
                rudp[2] = udp[0]; rudp[3] = udp[1];
                rudp[4] = (udp_total >> 8) & 0xFF;
                rudp[5] = udp_total & 0xFF;
                rudp[6] = 0; rudp[7] = 0;

                // DNS payload
                memcpy(&rudp[8], dns_resp, dr);

                send_ppp_frame(config, reply, 4 + ip_total, ppp_flag);
                ESP_LOGI(TAG, "DNS response sent (txn=0x%04X → 10.0.0.1)", txn_id);
            }
        }

    // Is this TCP (protocol=6) with enough bytes for the TCP header (20 bytes min after IP header)?
    } else if (protocol == 6 && ip_len >= (size_t)(ip_hdr_len + 20)) {
        // S5.5c: TCP — detect SYN and perform 3-way handshake stub.
        // TCP header: [0:2, src port] [2:4, dst port] [4:8, seq num] [8:12, ack num] [13:14, flags]
        // Flags: SYN=0x02, ACK=0x10, SYN+ACK=0x12, FIN=0x01, RST=0x04
        const unsigned char *tcp = &ip[ip_hdr_len];
        unsigned char tcp_flags = tcp[13];

        unsigned short src_port = ((unsigned short)tcp[0] << 8) | tcp[1];
        unsigned short dst_port = ((unsigned short)tcp[2] << 8) | tcp[3];
        unsigned long  seq_num  = ((unsigned long)tcp[4] << 24) | ((unsigned long)tcp[5] << 16)
                                | ((unsigned long)tcp[6] << 8)  | tcp[7];

        if (tcp_flags == 0x02) {
            // SYN received — respond with SYN-ACK to complete step 1 of the
            // TCP 3-way handshake (SYN → SYN-ACK → ACK).
            ESP_LOGI(TAG, "TCP SYN received: %u.%u.%u.%u:%u → :%u seq=%lu",
                     ip[12], ip[13], ip[14], ip[15],
                     src_port, dst_port, seq_num);

            // Build SYN-ACK reply:
            //   - Swap src/dst IP and ports
            //   - Our seq = 5000 (arbitrary), ack = their seq + 1
            //   - Flags = SYN+ACK (0x12)
            //
            // Reply IP+TCP layout:
            //   [0:4, PPP hdr] [4:24, IP hdr (swapped IPs)] [24:44, TCP hdr (swapped ports, SYN-ACK)]
            unsigned char reply[512];
            size_t tcp_hdr_len = 20;
            size_t ip_total = ip_hdr_len + tcp_hdr_len;
            if (ip_total > sizeof(reply) - 4) return;

            // PPP header for IPv4
            reply[0] = 0xFF; reply[1] = 0x03;
            reply[2] = 0x00; reply[3] = 0x21;

            // Copy and modify IP header: swap src/dst, set length.
            memcpy(&reply[4], ip, ip_hdr_len);
            unsigned char *rip = &reply[4];
            unsigned char tmp[4];
            memcpy(tmp, &rip[12], 4);
            memcpy(&rip[12], &rip[16], 4);
            memcpy(&rip[16], tmp, 4);
            rip[2] = (ip_total >> 8) & 0xFF;
            rip[3] = ip_total & 0xFF;
            rip[8] = 0x40;  // TTL = 64
            rip[10] = 0; rip[11] = 0;
            unsigned short ck = ip_checksum(rip, ip_hdr_len);
            rip[10] = (ck >> 8) & 0xFF;
            rip[11] = ck & 0xFF;

            // Build TCP header: swap ports, set SYN-ACK, our seq, their seq+1.
            // [0:2, src port] [2:4, dst port] [4:8, seq=5000] [8:12, ack=seq+1]
            // [12:13, data offset=5 words] [13:14, flags=0x12] [14:16, window]
            // [16:18, checksum] [18:20, urgent=0]
            unsigned char *rtcp = &rip[ip_hdr_len];
            unsigned long our_seq = 5000;
            unsigned long ack_num = seq_num + 1;
            memset(rtcp, 0, tcp_hdr_len);
            rtcp[0] = (dst_port >> 8) & 0xFF;
            rtcp[1] = dst_port & 0xFF;
            rtcp[2] = (src_port >> 8) & 0xFF;
            rtcp[3] = src_port & 0xFF;
            rtcp[4] = (our_seq >> 24) & 0xFF;
            rtcp[5] = (our_seq >> 16) & 0xFF;
            rtcp[6] = (our_seq >> 8)  & 0xFF;
            rtcp[7] = our_seq & 0xFF;
            rtcp[8]  = (ack_num >> 24) & 0xFF;
            rtcp[9]  = (ack_num >> 16) & 0xFF;
            rtcp[10] = (ack_num >> 8)  & 0xFF;
            rtcp[11] = ack_num & 0xFF;
            rtcp[12] = 0x50;  // data offset = 5 (20 bytes), no options
            rtcp[13] = 0x12;  // flags = SYN + ACK
            rtcp[14] = 0x16; rtcp[15] = 0x80; // window = 5760

            // TCP checksum uses a pseudo-header: src IP, dst IP, zero, proto, TCP length.
            // [0:4, src IP] [4:8, dst IP] [8:9, zero] [9:10, proto=6] [10:12, TCP len]
            unsigned char pseudo[12];
            memcpy(&pseudo[0], &rip[12], 4);
            memcpy(&pseudo[4], &rip[16], 4);
            pseudo[8] = 0; pseudo[9] = 6;
            pseudo[10] = (tcp_hdr_len >> 8) & 0xFF;
            pseudo[11] = tcp_hdr_len & 0xFF;

            unsigned long tcp_sum = 0;
            for (int i = 0; i < 12; i += 2)
                tcp_sum += ((unsigned short)pseudo[i] << 8) | pseudo[i + 1];
            for (size_t i = 0; i < tcp_hdr_len; i += 2)
                tcp_sum += ((unsigned short)rtcp[i] << 8) | ((i + 1 < tcp_hdr_len) ? rtcp[i + 1] : 0);
            while (tcp_sum >> 16)
                tcp_sum = (tcp_sum & 0xFFFF) + (tcp_sum >> 16);
            unsigned short tcp_ck = (unsigned short)(~tcp_sum);
            rtcp[16] = (tcp_ck >> 8) & 0xFF;
            rtcp[17] = tcp_ck & 0xFF;

            send_ppp_frame(config, reply, 4 + ip_total, ppp_flag);
            ESP_LOGI(TAG, "TCP SYN-ACK sent (our_seq=%lu, ack=%lu)", our_seq, ack_num);

        } else if (tcp_flags & 0x01) {
            // FIN received — respond with FIN-ACK to close the connection.
            // Same packet construction pattern as SYN-ACK but with flags=FIN+ACK (0x11).
            ESP_LOGI(TAG, "TCP FIN received from port %u", src_port);

            unsigned char reply[512];
            size_t tcp_hdr_len = 20;
            size_t ip_total = ip_hdr_len + tcp_hdr_len;
            if (ip_total > sizeof(reply) - 4) return;

            reply[0] = 0xFF; reply[1] = 0x03;
            reply[2] = 0x00; reply[3] = 0x21;

            // IP header: swap src/dst.
            memcpy(&reply[4], ip, ip_hdr_len);
            unsigned char *rip = &reply[4];
            unsigned char tmp[4];
            memcpy(tmp, &rip[12], 4);
            memcpy(&rip[12], &rip[16], 4);
            memcpy(&rip[16], tmp, 4);
            rip[2] = (ip_total >> 8) & 0xFF;
            rip[3] = ip_total & 0xFF;
            rip[8] = 0x40;
            rip[10] = 0; rip[11] = 0;
            unsigned short ck = ip_checksum(rip, ip_hdr_len);
            rip[10] = (ck >> 8) & 0xFF;
            rip[11] = ck & 0xFF;

            // TCP header: swap ports, ack their seq+1, set FIN+ACK.
            // [13:14, flags=0x11 FIN+ACK]
            unsigned char *rtcp = &rip[ip_hdr_len];
            unsigned long ack_num = seq_num + 1;
            memset(rtcp, 0, tcp_hdr_len);
            rtcp[0] = (dst_port >> 8) & 0xFF;
            rtcp[1] = dst_port & 0xFF;
            rtcp[2] = (src_port >> 8) & 0xFF;
            rtcp[3] = src_port & 0xFF;
            rtcp[4] = 0; rtcp[5] = 0; rtcp[6] = 0; rtcp[7] = 1; // seq = 1
            rtcp[8]  = (ack_num >> 24) & 0xFF;
            rtcp[9]  = (ack_num >> 16) & 0xFF;
            rtcp[10] = (ack_num >> 8)  & 0xFF;
            rtcp[11] = ack_num & 0xFF;
            rtcp[12] = 0x50;
            rtcp[13] = 0x11;  // FIN + ACK
            rtcp[14] = 0x16; rtcp[15] = 0x80;

            // TCP checksum with pseudo-header.
            unsigned char pseudo[12];
            memcpy(&pseudo[0], &rip[12], 4);
            memcpy(&pseudo[4], &rip[16], 4);
            pseudo[8] = 0; pseudo[9] = 6;
            pseudo[10] = (tcp_hdr_len >> 8) & 0xFF;
            pseudo[11] = tcp_hdr_len & 0xFF;

            unsigned long tcp_sum = 0;
            for (int i = 0; i < 12; i += 2)
                tcp_sum += ((unsigned short)pseudo[i] << 8) | pseudo[i + 1];
            for (size_t i = 0; i < tcp_hdr_len; i += 2)
                tcp_sum += ((unsigned short)rtcp[i] << 8) | ((i + 1 < tcp_hdr_len) ? rtcp[i + 1] : 0);
            while (tcp_sum >> 16)
                tcp_sum = (tcp_sum & 0xFFFF) + (tcp_sum >> 16);
            unsigned short tcp_ck = (unsigned short)(~tcp_sum);
            rtcp[16] = (tcp_ck >> 8) & 0xFF;
            rtcp[17] = tcp_ck & 0xFF;

            send_ppp_frame(config, reply, 4 + ip_total, ppp_flag);
            ESP_LOGI(TAG, "TCP FIN-ACK sent (ack=%lu)", ack_num);
        } else {
            ESP_LOGI(TAG, "TCP segment: flags=0x%02X src_port=%u dst_port=%u",
                     tcp_flags, src_port, dst_port);
        }

    } else {
        ESP_LOGI(TAG, "IPv4 protocol %u — not handled", protocol);
    }
}

static void handle_ppp_data_mode(sim_modem_config_t *config)
{
    // Byte-oriented handler: PPP is binary. We read raw bytes, unstuff (RFC 1662),
    // accumulate until 0x7E delimiter, then dispatch by protocol.
    unsigned char byte = 0;
    unsigned char frame_buf[512];
    size_t frame_len = 0;

    // Unstuffing state: 0x7D on wire means "next byte XOR 0x20". Real modems
    // receive stuffed bytes from the MCU and unstuff before parsing.
    int in_escaped = 0;

    // S5.6: Track consecutive '+' for escape (return to AT mode).
    int plus_count = 0;

    ESP_LOGI(TAG, "Entered PPP data mode — collecting HDLC frames");

    while (config->state == SIM_MODEM_STATE_DATA_MODE) {
        // S5.0: Read raw bytes from UART (PPP is binary, not line-based).
        int got = uart_read_bytes(config->uart_num, &byte, 1, pdMS_TO_TICKS(200));
        if (got <= 0) {
            // Idle timeout: no bytes right now.
            // S5.6: if we saw 3 '+' and then silence, escape to AT mode.
            if (plus_count >= 3) {
                ESP_LOGI(TAG, "+++ escape detected — returning to AT command mode");
                config->state = SIM_MODEM_STATE_PDP_ACTIVE;
                return;
            }
            continue;
        }

        // S5.6: track '+' bytes for escape sequence.
        if (byte == '+') {
            plus_count++;
            if (plus_count < 3) continue;
            // Don't act on 3rd '+' yet — wait for silence (timeout above).
            continue;
        } else {
            plus_count = 0;
        }

        // --- Byte unstuffing (RFC 1662) — real modem behavior ---
        // The MCU (lwIP) sends stuffed bytes: 0x7D 0x5E = 0x7E, 0x7D 0x5D = 0x7D.
        // We unstuff before treating 0x7E as frame boundary or accumulating.
        if (in_escaped) {
            byte ^= PPP_TRANS_BYTE;
            in_escaped = 0;
        } else if (byte == PPP_ESCAPE_BYTE) {
            in_escaped = 1;
            continue;
        }

        // S5.1: Frame delimiter — 0x7E (after unstuffing) marks frame boundaries.
        if (byte == PPP_FLAG_BYTE) {
            if (frame_len == 0) {
                continue;
            }

            // S5.2: Frame dispatch. PPP header: Address(0xFF), Control(0x03), Protocol(2B).
            if (frame_len >= 4 && frame_buf[0] == 0xFF && frame_buf[1] == 0x03) {
                unsigned short protocol = ((unsigned short)frame_buf[2] << 8) | frame_buf[3];

                if (protocol == 0xC021) {
                    handle_lcp_frame(config, frame_buf, frame_len, PPP_FLAG_BYTE);
                } else if (protocol == 0x8021) {
                    handle_ipcp_frame(config, frame_buf, frame_len, PPP_FLAG_BYTE);
                } else if (protocol == 0x0021) {
                    handle_ipv4_packet(config, frame_buf, frame_len, PPP_FLAG_BYTE);
                } else {
                    ESP_LOGI(TAG, "PPP frame: unhandled proto=0x%04X (len=%u)",
                             protocol, (unsigned int)frame_len);
                }
            } else {
                ESP_LOGW(TAG, "PPP frame too short/unknown header (len=%u)",
                         (unsigned int)frame_len);
            }

            frame_len = 0;
            continue;
        }

        if (frame_len < sizeof(frame_buf)) {
            frame_buf[frame_len++] = byte;
        } else {
            ESP_LOGW(TAG, "PPP frame overflow; dropping partial frame");
            frame_len = 0;
        }
    }
}


// ============================================================================
//  PUBLIC API
// ============================================================================

void sim_modem_init(void)
{
    sim_modem_config_t *config = &s_sim_modem_cfg;
    config->state = SIM_MODEM_STATE_OFF;

    // 2. Describe the UART's electrical parameters: baud rate, word format,
    //    and flow control. This struct doesn't touch hardware yet — it's just
    //    a settings object that uart_param_config() will apply to the peripheral.
    //    8N1 (8 data bits, no parity, 1 stop bit) is the universal default for
    //    AT command interfaces — every cellular modem uses it.
    uart_config_t uart_cfg = {
        .baud_rate           = config->baud_rate,
        .data_bits           = UART_DATA_8_BITS, // <-- various defined types in uart.h
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = config->flow_control
                                   ? UART_HW_FLOWCTRL_CTS_RTS
                                   : UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
    };

    // 3. Apply the config to the UART peripheral registers. After this call,
    //    the hardware knows the baud rate, word format, and flow control mode,
    //    but no pins are assigned yet and no driver buffers exist — the UART
    //    can't actually send or receive until steps 4 and 5 complete.
    uart_param_config(config->uart_num, &uart_cfg);

    // 4. Route the UART's TX, RX, RTS, and CTS signals to physical GPIO pins
    //    via the ESP32's GPIO matrix. Without this, the UART has no electrical
    //    connection — bytes written would go nowhere. When flow control is off,
    //    rts_pin and cts_pin are -1 (UART_PIN_NO_CHANGE), so those signals
    //    stay unconnected and the UART ignores them.
    uart_set_pin(config->uart_num,
                 config->tx_pin, config->rx_pin,
                 config->rts_pin, config->cts_pin);

    // 5. Install the UART driver: allocate RX and TX ring buffers (1024-byte) 
    //    and register the driver with the ESP-IDF UART layer. This call allows uart_*
    //    read/write calls to be used, which abstracts away the direct register reading
    //    and polling you have to do to read from UART otherwise
    uart_driver_install(config->uart_num, 1024, 1024, 0, NULL, 0);

    ESP_LOGI(TAG, "sim_modem_init: uart=%d tx=%d rx=%d baud=%d",
             config->uart_num, config->tx_pin, config->rx_pin,
             config->baud_rate);
}

void sim_modem_task(void *param)
{
    (void)param;
    sim_modem_config_t *config = &s_sim_modem_cfg;
    char line_buf[256];

    // -----------------------------------------------------------------------
    // Step 1: Simulate modem power-on sequence.
    // Real modems need time before they can accept AT commands, so we emulate
    // that boot window to keep driver timing behavior realistic.
    // -----------------------------------------------------------------------
    vTaskDelay(pdMS_TO_TICKS(1500));
    config->state = SIM_MODEM_STATE_READY;
    ESP_LOGI(TAG, "Modem powered on, state=READY");

    ESP_LOGI(TAG, "sim_modem_task started");

    while (1) {
        // -------------------------------------------------------------------
        // Step 2: Main AT command loop
        // Keep servicing AT commands until the modem is told to switch into
        // PPP data mode (ATD*99#). A read timeout is normal when idle.
        // -------------------------------------------------------------------
        while (config->state != SIM_MODEM_STATE_DATA_MODE) {
            // Basically busy wait for commands
            int line_len = read_line(config, line_buf, sizeof(line_buf), pdMS_TO_TICKS(500));

            // Test if command found this cycle
            if (line_len <= 0) {
                // Timeout/no data: stay responsive but do nothing this cycle.
                continue;
            }

            // Handle command
            config->state = handle_at_command(config, line_buf);
        }

        // -------------------------------------------------------------------
        // Step 3: Enter PPP data mode.
        // This function blocks while in DATA_MODE. If it returns (e.g. because
        // an escape sequence switched state), we loop back to Step 2.
        // -------------------------------------------------------------------
        handle_ppp_data_mode(config);
    }
}
