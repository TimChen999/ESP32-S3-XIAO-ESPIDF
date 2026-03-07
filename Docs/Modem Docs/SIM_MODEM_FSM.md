# Sim Modem — Complete FSM Reference

Every state, transition, branch, input, and output in `sim_modem.c`,
documented as a formal finite state machine.

**Note:** All states are held in `config->state` inside the sim_modem; this is internal to the sim only. The modem driver never sees it and infers modem state from AT responses (e.g. OK, +CPIN: READY, +CREG: 0,1, CONNECT).

---

## Top-Level State Enum

```
SIM_MODEM_STATE_OFF        → Modem has not booted yet
SIM_MODEM_STATE_READY      → Powered on, will accept AT commands
SIM_MODEM_STATE_SIM_READY  → SIM card validated
SIM_MODEM_STATE_REGISTERED → Registered on cellular network
SIM_MODEM_STATE_PDP_ACTIVE → Data bearer (PDP context) is active
SIM_MODEM_STATE_DATA_MODE  → PPP binary frames on UART (no more AT)
```

---

## Step-by-step overview (what actually happens)

1. **S0 → S1:** Init sets state OFF and configures UART; the task runs a 1.5s boot delay, then sets state READY and enters the AT loop.
2. **S1 (READY):** Modem reads text lines, echoes (if on), and replies to AT commands. Only AT+CPIN? causes a state change → SIM_READY.
3. **S2 (SIM_READY):** Same as READY, but AT+CREG?/AT+CEREG? now return “registered”; when the driver sends one of these → REGISTERED.
4. **S3 (REGISTERED):** Driver sends AT+CGACT=1,1; we respond OK → PDP_ACTIVE.
5. **S4 (PDP_ACTIVE):** Driver sends ATD*99#; we respond CONNECT and set state DATA_MODE; the AT loop exits and we enter handle_ppp_data_mode().
6. **S5 (DATA_MODE):** We read raw bytes, detect 0x7E frames and +++ escape. LCP (S5.3) and IPCP (S5.4) set up the link and IPs; IPv4 (S5.5) handles ping, DNS, and TCP SYN/FIN. When +++ is detected with guard time, state goes back to PDP_ACTIVE and we return to the AT loop.

---

## S0: OFF (Initial State)

**High-level:** The modem is not running yet. Init has set state to OFF and configured the UART (baud, pins, flow control, driver install). The task may not have started, or it is in the initial 1.5s boot delay. No AT commands are processed. **What changes:** When the boot delay expires, state becomes READY and the modem starts accepting AT commands.

```
Set by:       sim_modem_init()
Entry action: config->state = SIM_MODEM_STATE_OFF
              Configure UART (baud, pins, flow control, driver install)
Input:        Config struct (uart_num, tx_pin, rx_pin, baud_rate, flow_control)
Output:       UART peripheral is live and ready
```

| Transition | Condition | Next State |
|---|---|---|
| → S1 | sim_modem_task starts, 1.5s boot delay expires | READY |

**Real modem errors the sim doesn't produce at this stage:**

| Error | What happens on real hardware | What driver should do |
|---|---|---|
| No response to `AT` at all | Modem didn't power on (bad supply, enable pin not asserted, hardware fault) | Retry `AT` with backoff for 10-30s, then report hardware failure |
| Boot takes 5-15s (not 1.5s) | Real modems have variable boot times depending on firmware, SIM, and network scan | Driver must tolerate longer timeouts on first `AT` |
| Modem sends unsolicited `+CFUN: 1` | Power-on notification before it's ready for commands | Driver should ignore/absorb unexpected URCs during boot |
| Baud rate mismatch | Modem defaults to a different baud (e.g. 9600) than driver expects | Auto-baud detection or AT at multiple rates |

---

## S1: READY (Modem Booted)

**High-level:** The modem has “booted” and is in AT command mode. It reads text lines from UART, parses each line as an AT command, and sends back text responses. Echo is on by default (commands are echoed before the reply). The driver can ping (AT), disable echo (ATE0), check signal (AT+CSQ), query operator (AT+COPS?), and define PDP context (AT+CGDCONT). Registration and dial commands are rejected until later states. **What changes:** The only state transition from READY is when the driver sends AT+CPIN? and we respond with +CPIN: READY → state becomes SIM_READY.

```
Set by:       sim_modem_task after vTaskDelay(1500ms)
Entry action: config->state = SIM_MODEM_STATE_READY
              Log "Modem powered on, state=READY"
Input:        AT command strings from UART RX (via read_line)
Output:       AT response strings to UART TX (via send_response)
```

The modem now sits in the AT command loop. Each iteration:

1. Call `read_line()` with 500ms timeout
2. If timeout (line_len <= 0) → stay in loop, do nothing
3. If line received → pass to `handle_at_command()`
4. If echo_enabled → echo command back before responding

### AT Command Branches in READY State

| Input | Branch Condition | Output (UART TX) | State Change |
|---|---|---|---|
| `"AT"` | Always valid | `"\r\nOK\r\n"` | None |
| `"ATE0"` | Always valid | `"\r\nOK\r\n"` | None (echo_enabled = 0) |
| `"ATE1"` | Always valid | `"\r\nOK\r\n"` | None (echo_enabled = 1) |
| `"AT+CPIN?"` | state >= READY (true) | `"\r\n+CPIN: READY\r\nOK\r\n"` | → S2 (SIM_READY) |
| `"AT+CSQ"` | Always valid | `"\r\n+CSQ: 20,0\r\nOK\r\n"` | None |
| `"AT+CREG?"` | state >= SIM_READY (false) | `"\r\n+CREG: 0,0\r\nOK\r\n"` | None |
| `"AT+CEREG?"` | state >= SIM_READY (false) | `"\r\n+CEREG: 0,0\r\nOK\r\n"` | None |
| `"AT+COPS?"` | Always valid | `"\r\n+COPS: 0,0,\"Fake Cellular\"\r\nOK\r\n"` | None |
| `"AT+CGDCONT=..."` | Always valid | `"\r\nOK\r\n"` | None |
| `"AT+CGACT=1,1"` | state >= REGISTERED (false) | `"\r\nERROR\r\n"` | None |
| `"ATD*99#"` | state == PDP_ACTIVE (false) | `"\r\nNO CARRIER\r\n"` | None |
| (unknown) | Catch-all | `"\r\nERROR\r\n"` | None |

**Real modem errors the sim doesn't produce at this stage:**

| Error | What happens on real hardware | What driver should do |
|---|---|---|
| `AT` returns garbled text | UART noise, baud mismatch, or modem mid-boot | Flush RX buffer, retry `AT` |
| `AT+CPIN?` → `+CPIN: SIM PIN` | SIM card requires a PIN to unlock | Send `AT+CPIN="1234"` with the PIN, or fail if unknown |
| `AT+CPIN?` → `+CPIN: SIM PUK` | SIM locked after too many wrong PINs — needs PUK code | Fatal error — cannot proceed without user intervention |
| `AT+CPIN?` → `+CME ERROR: 10` | SIM not inserted | Fatal error — log and stop |
| `AT+CPIN?` → `+CME ERROR: 13` | SIM failure (damaged/unreadable) | Fatal error — log and stop |
| Modem echoes garbage before `OK` | Echo enabled + line noise = corrupted echo | Driver should strip echo (match sent command) before parsing response |

---

## S2: SIM_READY

**High-level:** The SIM is considered present and ready (we already responded to AT+CPIN? with +CPIN: READY). We still process all the same AT commands as READY, but now AT+CREG? and AT+CEREG? return “registered” (0,1) instead of “not registered” (0,0). The driver typically polls CREG/CEREG here to confirm network registration. **What changes:** When the driver sends AT+CREG? or AT+CEREG?, we respond with +CREG: 0,1 (or CEREG equivalent) and state becomes REGISTERED.

```
Set by:       handle_at_command when AT+CPIN? received in state >= READY
Entry action: config->state = SIM_MODEM_STATE_SIM_READY
Input:        AT command strings
Output:       AT response strings
```

### AT Command Branches in SIM_READY State

Everything from S1 still works, plus these transitions unlock:

| Input | Branch Condition | Output | State Change |
|---|---|---|---|
| `"AT+CREG?"` | state >= SIM_READY (true) | `"\r\n+CREG: 0,1\r\nOK\r\n"` | → S3 (REGISTERED) |
| `"AT+CEREG?"` | state >= SIM_READY (true) | `"\r\n+CEREG: 0,1\r\nOK\r\n"` | → S3 (REGISTERED) |
| `"AT+CGACT=1,1"` | state >= REGISTERED (false) | `"\r\nERROR\r\n"` | None |
| `"ATD*99#"` | state == PDP_ACTIVE (false) | `"\r\nNO CARRIER\r\n"` | None |

**Real modem errors the sim doesn't produce at this stage:**

| Error | What happens on real hardware | What driver should do |
|---|---|---|
| `AT+CREG?` → `+CREG: 0,2` | Searching for network — not registered yet | Wait 2-5s, poll again, retry up to 60s |
| `AT+CREG?` → `+CREG: 0,3` | Registration denied (banned IMEI, invalid SIM for this network) | Fatal error — cannot register on this network |
| `AT+CREG?` → `+CREG: 0,0` | Not registered and not searching (antenna issue, no coverage) | Retry after delay, or try manual operator selection with `AT+COPS=1` |
| `AT+CSQ` → `+CSQ: 99,99` | No signal / unknown signal quality | Log weak signal, may cause registration to fail or take longer |
| `AT+CSQ` → `+CSQ: 5,0` | Very weak signal (marginal coverage) | Proceed but expect slow data, frequent drops |
| Unsolicited `+CREG: 0,0` | Network lost after being registered (moved out of coverage, tower failure) | Re-enter registration polling, tear down PDP if active |

---

## S3: REGISTERED

**High-level:** The modem is “registered on the cellular network” (cell tower). All previous AT commands still work. The new capability is activating the data bearer: the driver sends AT+CGACT=1,1 to bring up the PDP context so that a data call (ATD*99#) can be placed. **What changes:** When the driver sends AT+CGACT=1,1 we respond with OK and state becomes PDP_ACTIVE. ATD*99# is still rejected until we are in PDP_ACTIVE.

```
Set by:       handle_at_command when AT+CREG? or AT+CEREG? received in state >= SIM_READY
Entry action: config->state = SIM_MODEM_STATE_REGISTERED
Input:        AT command strings
Output:       AT response strings
```

### AT Command Branches in REGISTERED State

| Input | Branch Condition | Output | State Change |
|---|---|---|---|
| `"AT+CGACT=1,1"` | state >= REGISTERED (true) | `"\r\nOK\r\n"` | → S4 (PDP_ACTIVE) |
| `"ATD*99#"` | state == PDP_ACTIVE (false) | `"\r\nNO CARRIER\r\n"` | None |

All other commands behave the same as S1/S2.

**Real modem errors the sim doesn't produce at this stage:**

| Error | What happens on real hardware | What driver should do |
|---|---|---|
| `AT+CGACT=1,1` → `+CME ERROR: 30` | No network service (lost registration between CREG and CGACT) | Re-check registration with `AT+CREG?`, re-register if needed |
| `AT+CGACT=1,1` → `+CME ERROR: 33` | Requested service option not subscribed (SIM/plan doesn't allow data) | Fatal for data — log and report to user |
| `AT+CGACT=1,1` → `+CME ERROR: 148` | Unspecified GPRS error | Retry once, then fail |
| `AT+CGDCONT` → `ERROR` | Invalid APN syntax or too many PDP contexts defined | Check APN string format, use context ID 1 |
| PDP activation takes 5-30s | Network-side provisioning delay (real carriers are slow) | Use longer timeout (30s+) for CGACT |

---

## S4: PDP_ACTIVE

**High-level:** The data bearer (PDP context) is active. The modem is still in AT command mode but is now allowed to “dial” for data. When the driver sends ATD*99#, we respond with CONNECT and switch to DATA_MODE. **What changes:** On ATD*99#, we set state to DATA_MODE, the main AT loop exits (the `while (state != DATA_MODE)` condition fails), and we call handle_ppp_data_mode(). From the driver’s perspective, the UART switches from text AT to binary PPP frames (0x7E-delimited).

```
Set by:       handle_at_command when AT+CGACT=1,1 received in state >= REGISTERED
Entry action: config->state = SIM_MODEM_STATE_PDP_ACTIVE
Input:        AT command strings
Output:       AT response strings
```

### AT Command Branches in PDP_ACTIVE State

| Input | Branch Condition | Output | State Change |
|---|---|---|---|
| `"ATD*99#"` | state == PDP_ACTIVE (true) | `"\r\nCONNECT 115200\r\n"` | → S5 (DATA_MODE) |

**On transition to DATA_MODE:**
- AT command loop (`while state != DATA_MODE`) exits
- `handle_ppp_data_mode()` is called
- UART switches from text AT to binary PPP

**Real modem errors the sim doesn't produce at this stage:**

| Error | What happens on real hardware | What driver should do |
|---|---|---|
| `ATD*99#` → `NO CARRIER` | PDP context dropped between CGACT and ATD (network hiccup) | Re-activate PDP with `AT+CGACT=1,1`, then retry dial |
| `ATD*99#` → `BUSY` | Modem already in a call/data session | Send `ATH` to hang up, then retry |
| `ATD*99#` → `CONNECT` then immediate `NO CARRIER` | PPP negotiation failed on the modem's network side | Re-dial, possibly re-activate PDP |
| `CONNECT` arrives after 10-20s | Network-side PPP setup is slow | Use long timeout (30s) for the CONNECT response |
| Unsolicited `NO CARRIER` during DATA_MODE | Network dropped the data connection (coverage loss, idle timeout) | Detect in PPP byte loop, tear down PPP, re-enter AT mode |

---

## S5: DATA_MODE (PPP Phase)

**High-level:** We are no longer in AT mode. The modem reads raw bytes from the UART, looks for 0x7E frame boundaries and the +++ escape sequence. Complete frames are dispatched by PPP protocol ID: LCP (link negotiation), IPCP (IP address assignment), and IPv4 (ICMP ping, UDP/DNS, TCP handshake). **What changes:** LCP and IPCP establish the link and assign IPs; then IPv4 traffic is processed (echo reply, DNS reply, SYN-ACK, FIN-ACK). When the driver sends +++ with guard time, we set state back to PDP_ACTIVE, handle_ppp_data_mode() returns, and the outer task re-enters the AT command loop.

```
Set by:       handle_at_command when ATD*99# received in state == PDP_ACTIVE
Entry action: config->state = SIM_MODEM_STATE_DATA_MODE
              Enter handle_ppp_data_mode()
Input:        Raw binary bytes from UART RX
Output:       PPP frames to UART TX (0x7E-delimited)
```

DATA_MODE is a composite state containing its own internal sub-FSM.
The modem reads one byte at a time and runs through the following
decision tree on every byte:

---

### S5.0: Byte Read Loop

**High-level:** We read one byte at a time from the UART. Each byte is either part of the +++ escape (count consecutive '+'), a PPP escape sequence (0x7D byte-stuffing per RFC 1662), part of a PPP frame (accumulate until 0x7E), or filler. **What changes:** Three '+' followed by a timeout triggers escape (state → PDP_ACTIVE, return). Escape bytes (0x7D) set an `in_escaped` flag so the next byte is XOR'd with 0x20 (unstuffed) before processing. A 0x7E with data already in the buffer completes a frame and we dispatch it (S5.2). Any other byte either resets the plus counter or is appended to the frame buffer.

```
Input:        Single byte from uart_read_bytes (200ms timeout)
State:        in_escaped flag (for RFC 1662 byte unstuffing)
```

| Condition | Action | Next |
|---|---|---|
| Timeout (got <= 0), plus_count >= 3 | Escape detected → state = PDP_ACTIVE, return | → S4 |
| Timeout (got <= 0), plus_count < 3 | Continue waiting | → S5.0 |
| byte == '+' | plus_count++; if < 3 continue | → S5.0 |
| byte == '+', plus_count reaches 3 | Wait for silence (next timeout) | → S5.0 |
| byte != '+' | plus_count = 0; fall through to unstuffing | |
| in_escaped == true | byte ^= 0x20 (unstuff); in_escaped = false; fall through | → S5.1 |
| byte == 0x7D (PPP escape byte) | in_escaped = true; continue (consume escape marker) | → S5.0 |
| other byte | Fall through directly | → S5.1 |

---

### S5.1: Frame Delimiter Check

**High-level:** For each non-'+' byte we decide: empty 0x7E (skip), closing 0x7E (frame complete → dispatch), or payload (accumulate). **What changes:** frame_len and frame_buf are updated; when a closing 0x7E is seen we pass the buffer to S5.2 and then reset for the next frame.

```
Input:        Current byte (not '+')
```

| Condition | Action | Next |
|---|---|---|
| byte == 0x7E, frame_len == 0 | Empty frame (inter-frame fill), skip | → S5.0 |
| byte == 0x7E, frame_len > 0 | Complete frame collected → dispatch | → S5.2 |
| byte != 0x7E, frame_len < 512 | Accumulate: frame_buf[frame_len++] = byte | → S5.0 |
| byte != 0x7E, frame_len >= 512 | Overflow: drop frame, reset frame_len = 0 | → S5.0 |

---

### S5.2: Frame Dispatch

**High-level:** We have a complete PPP frame. We check the header (0xFF, 0x03) and the 16-bit protocol ID, then hand the payload to the right handler. **What changes:** LCP (0xC021) → S5.3, IPCP (0x8021) → S5.4, IPv4 (0x0021) → S5.5. Invalid header or unknown protocol is logged and the frame is discarded; control returns to the byte loop (S5.0).

```
Input:        Complete PPP frame in frame_buf[0..frame_len-1]
              PPP header: [0:1, addr] [1:2, ctrl] [2:4, protocol]
```

| Condition | Action | Next |
|---|---|---|
| frame_len < 4 or buf[0] != 0xFF or buf[1] != 0x03 | Log warning, discard | → S5.0 |
| protocol == 0xC021 | → S5.3 (LCP handler) | → S5.0 |
| protocol == 0x8021 | → S5.4 (IPCP handler) | → S5.0 |
| protocol == 0x0021 | → S5.5 (IPv4 handler) | → S5.0 |
| protocol == other | Log unhandled proto, discard | → S5.0 |

---

### S5.3: LCP Handler (handle_lcp_frame)

**High-level:** Layer 2 link setup. We receive the driver’s LCP Config-Request, send Config-Ack (accept its options), and send our own Config-Request (MRU=1500). No state variable changes in the sim—we just send two frames. **What changes:** The link is considered configured from our side; the driver will Ack our request and then start IPCP.

```
Networking layer: Layer 2 — Data Link
Purpose:          Negotiate PPP link parameters before IP can flow
Input:            LCP frame: [4:5, code] [5:6, id] [6:8, length] [8:..., options]
```

| Condition | Action | Output |
|---|---|---|
| frame_len < 8 | Log "too short" | None |
| code == 0x01, valid length | **Two outputs:** | |
| | 1. Send Config-Ack (echo driver's options back with code=0x02) | `[0x7E] [0xFF 0x03 0xC021] [0x02 id len options] [0x7E]` |
| | 2. Send our Config-Request (MRU=1500) | `[0x7E] [0xFF 0x03 0xC021] [0x01 id 0x0008 0x01 0x04 0x05DC] [0x7E]` |
| code == 0x05, valid length | Send Terminate-Ack (echo back with code=0x06) | `[0x7E] [0xFF 0x03 0xC021] [0x06 id len data] [0x7E]` |
| other code or invalid length | Log "unhandled code" | None |

**Config-Ack frame structure:**

```
[0:1, 0xFF addr] [1:2, 0x03 ctrl] [2:4, 0xC021 LCP]
[4:5, 0x02 Config-Ack] [5:6, same id] [6:8, same length]
[8:8+opts_len, driver's options echoed verbatim]
```

**Our Config-Request frame structure:**

```
[0:4, 0xFF/0x03/0xC021 PPP hdr]
[4:5, 0x01 Config-Request] [5:6, our id] [6:8, length=8]
[8:12, MRU option: type=0x01 len=4 val=0x05DC(1500)]
```

**Real modem errors the sim doesn't produce at this stage:**

| Error | What happens on real hardware | What driver should do |
|---|---|---|
| Config-Reject (code 0x04) | Modem rejects an option it doesn't support (e.g. compression) | Remove rejected option, resend Config-Request without it |
| Config-Nak (code 0x03) | Modem wants a different value for an option (e.g. smaller MRU) | Accept suggested value, resend Config-Request with it |
| No Config-Ack received | Modem never Acks our Config-Request (timeout) | Retransmit Config-Request up to 10 times, then fail |
| LCP Echo-Request (code 0x09) | Modem sends keepalive pings during established link | Must reply with Echo-Reply (code 0x0A) or modem closes link |

**Note:** The sim now implements byte-stuffing (RFC 1662: 0x7E→0x7D 0x5E, 0x7D→0x7D 0x5D, and chars < 0x20) and FCS (CRC-16) on sent frames, and unstuffs received frames. LCP Terminate-Request (code 0x05) is handled by the sim — it replies with Terminate-Ack (code 0x06).

---

### S5.4: IPCP Handler (handle_ipcp_frame)

**High-level:** Layer 3 address assignment. We assign the driver the IP 10.0.0.2 (via Config-Nak or Config-Ack) and tell the driver we are 10.0.0.1 with DNS 10.0.0.1 (via our Config-Request). **What changes:** Once both sides Ack, IP is “up”: the driver has 10.0.0.2, we have 10.0.0.1, and IPv4 packets can be exchanged.

```
Networking layer: Layer 3 — Network
Purpose:          Assign IP addresses (PPP equivalent of DHCP)
Input:            IPCP frame: [4:5, code] [5:6, id] [6:8, length] [8:..., options]
```

**Option scanning:** Walk TLV chain looking for IP-Address option (type=0x03, len=6).

| Condition | Action | Output |
|---|---|---|
| frame_len < 8 | Log "too short" | None |
| code != 0x01 AND code != 0x02 | Log unhandled code | None |
| code == 0x02 | Config-Ack from driver (accepted our IP) | Log "IP layer is up" |
| code == 0x01, IP option == 10.0.0.2 | Accept → send Config-Ack | Ack frame (proto=0x8021, code=0x02) |
| code == 0x01, IP option != 10.0.0.2 | Reject → send Config-Nak with 10.0.0.2 | Nak frame (proto=0x8021, code=0x03) |
| (after Ack or Nak) | Send our own Config-Request | Request with modem IP |

**Config-Nak frame (suggesting 10.0.0.2):**

```
[0:4, 0xFF/0x03/0x8021 PPP hdr]
[4:5, 0x03 Config-Nak] [5:6, id] [6:8, length=10]
[8:14, IP-Address option: type=0x03 len=6 val=10.0.0.2]
```

**Our Config-Request frame (modem IP only):**

```
[0:4, PPP hdr] [4:5, 0x01 Config-Req] [5:6, id] [6:8, length=10]
[8:14, IP-Address opt: type=0x03 len=6 val=10.0.0.1]
```

**Typical IPCP exchange sequence:**

```
Driver → Modem:  Config-Request (IP=0.0.0.0)
Modem  → Driver: Config-Nak (use 10.0.0.2)
Modem  → Driver: Config-Request (modem=10.0.0.1)
Driver → Modem:  Config-Request (IP=10.0.0.2)
Modem  → Driver: Config-Ack (accepted)
Driver → Modem:  Config-Ack (accepted modem's IP)
                  — IPCP is now open, IP traffic can flow —
```

**Real modem errors the sim doesn't produce at this stage:**

| Error | What happens on real hardware | What driver should do |
|---|---|---|
| IPCP Config-Reject | Modem rejects an option (e.g. doesn't support DNS option 0x81) | Remove rejected option, resend without it |
| IPCP negotiation loops | Driver and modem keep Nak'ing each other (incompatible IP ranges) | Cap retries, fail after 10 attempts |
| No IPCP Config-Ack received | Modem never accepts driver's IP (timeout) | Retransmit, then tear down LCP and redial |
| Modem assigns 0.0.0.0 | Network didn't assign an IP (carrier provisioning issue) | Treat as failure, retry PDP activation |
| IPCP happens before LCP is open | Protocol ordering violation (broken modem firmware) | Ignore IPCP frames until LCP is confirmed open |

---

### S5.5: IPv4 Handler (handle_ipv4_packet)

**High-level:** We have an IPv4 packet inside a PPP frame. We look at the protocol field and hand off to ICMP, UDP, or TCP. **What changes:** ICMP echo → we send an echo reply. UDP port 53 → we send a canned DNS response. TCP SYN → we send SYN-ACK; TCP FIN → we send FIN-ACK. Other protocols or malformed packets are dropped or logged.

```
Networking layer: Layers 3–7
Purpose:          Route incoming IP packets to protocol-specific handlers
Input:            IPv4 packet starting at frame_buf[4]
                  [0:1, ver+hdr_len] [2:4, total_len] [9:10, protocol]
                  [12:16, src IP] [16:20, dst IP]
```

| Condition | Action | Next |
|---|---|---|
| ip_len < 20 | Log "too short" | Return |
| protocol == 1 (ICMP) | → S5.5a | |
| protocol == 17 (UDP) | → S5.5b | |
| protocol == 6 (TCP) | → S5.5c | |
| protocol == other | Log "not handled" | Return |

---

### S5.5a: ICMP Echo Reply

**High-level:** The driver (or host) sent a ping (ICMP type 8). We swap IP src/dst, set type to 0 (Echo Reply), fix checksums, and send the packet back. **What changes:** One PPP frame out with the echo reply; no state change.

```
Networking layer: Layer 3
Input:            ICMP header at ip[ip_hdr_len]:
                  [0:1, type] [1:2, code] [2:4, checksum] [4:8, id+seq]
```

| Condition | Action | Output |
|---|---|---|
| icmp type != 8 | Ignore (not an echo request) | None |
| icmp type == 8 | Build echo reply | PPP frame with IPv4+ICMP |

**Reply construction:**

1. Copy entire incoming IP packet
2. Swap src/dst IP addresses
3. Zero IP checksum → recompute with `ip_checksum()`
4. Set ICMP type = 0 (Echo Reply)
5. Zero ICMP checksum → recompute with `ip_checksum()`
6. Wrap in PPP frame → `send_ppp_frame()`

---

### S5.5b: UDP / DNS Response

**High-level:** We only handle UDP to port 53 (DNS). We parse the query (txn ID, question), echo the question back, and append an A record for 10.0.0.1. **What changes:** One PPP frame out containing IP+UDP+DNS response; no state change.

```
Networking layer: Layer 4 (UDP) + Layer 7 (DNS)
Input:            UDP header at ip[ip_hdr_len]:
                  [0:2, src port] [2:4, dst port] [4:6, length] [6:8, checksum]
                  DNS payload at udp[8]:
                  [0:2, txn ID] [2:4, flags] [4:12, counts] [12:..., question]
```

| Condition | Action | Output |
|---|---|---|
| dst_port != 53 | Ignore (not DNS) | None |
| dst_port == 53, payload < 12 bytes | Ignore (too short) | None |
| dst_port == 53, valid query | Build DNS response | PPP frame with IP+UDP+DNS |

**DNS response construction:**

1. Parse txn ID from query
2. Walk QNAME labels to measure question section length
3. Build DNS response header (same txn ID, response flags, 1Q, 1A)
4. Echo question section verbatim
5. Append answer RR:
   - Name pointer `0xC00C` (back to QNAME)
   - Type A (1), Class IN (1)
   - TTL = 60 seconds
   - RDATA = 10.0.0.1 (4 bytes)
6. Wrap DNS in UDP (swap ports, set length, zero checksum)
7. Wrap UDP in IP (swap IPs, recompute IP checksum)
8. Wrap IP in PPP frame → `send_ppp_frame()`

**Real modem errors the sim doesn't produce at this stage (IP/ICMP/DNS):**

| Error | What happens on real hardware | What driver should do |
|---|---|---|
| ICMP Destination Unreachable (type 3) | Remote host or network unreachable | Log and report to application — the route doesn't exist |
| ICMP Time Exceeded (type 11) | TTL expired in transit (routing loop or too many hops) | Log, application should retry or use a different destination |
| DNS NXDOMAIN (rcode=3) | Domain name doesn't exist | Return lookup failure to application |
| DNS SERVFAIL (rcode=2) | DNS server error | Retry with secondary DNS, or fail |
| DNS timeout (no response) | DNS server unreachable or packet lost | Retry 2-3 times with increasing timeout, then fail |
| IP fragmentation | Packet larger than link MTU arrives in fragments | Reassemble before processing (lwIP handles this) |
| Bad IP checksum | Corrupted packet from UART noise or PPP framing error | Drop silently (lwIP drops these automatically) |

---

### S5.5c: TCP Handler

**High-level:** We only react to SYN (connection start) and FIN (connection end). For SYN we send SYN-ACK (seq=5000, ack=their seq+1). For FIN we send FIN-ACK. We do not keep connection state or handle data segments. **What changes:** One PPP frame out per SYN or FIN; no state change in the sim.

```
Networking layer: Layer 4
Input:            TCP header at ip[ip_hdr_len]:
                  [0:2, src port] [2:4, dst port] [4:8, seq num]
                  [8:12, ack num] [12:13, data offset] [13:14, flags]
                  [14:16, window] [16:18, checksum] [18:20, urgent]
```

| Condition | Action | Output |
|---|---|---|
| flags == 0x02 (SYN) | → S5.5c-SYN | SYN-ACK packet |
| flags & 0x01 (FIN) | → S5.5c-FIN | FIN-ACK packet |
| other flags | Log segment info | None |

#### S5.5c-SYN: SYN-ACK Response

```
Purpose: Complete step 1 of TCP 3-way handshake (SYN → SYN-ACK → ACK)
```

**SYN-ACK construction:**

1. Swap src/dst IP addresses
2. Swap src/dst TCP ports
3. Set our seq = 5000, ack = their seq + 1
4. Set flags = 0x12 (SYN + ACK)
5. Data offset = 5 (20 bytes, no options)
6. Window = 5760
7. Compute TCP checksum using pseudo-header:
   `[0:4, src IP] [4:8, dst IP] [8:9, zero] [9:10, proto=6] [10:12, TCP len]`
8. Compute IP checksum
9. Wrap in PPP frame → `send_ppp_frame()`

**SYN-ACK frame structure:**

```
[0:4, PPP hdr 0xFF/0x03/0x0021]
[4:24, IP header (swapped IPs, TTL=64, proto=6)]
[24:26, dst port (was src)] [26:28, src port (was dst)]
[28:32, seq=5000] [32:36, ack=their_seq+1]
[36:37, data offset=0x50] [37:38, flags=0x12]
[38:40, window=5760] [40:42, checksum] [42:44, urgent=0]
```

#### S5.5c-FIN: FIN-ACK Response

```
Purpose: Acknowledge connection teardown
```

**FIN-ACK construction:**

1. Same pattern as SYN-ACK but:
   - Flags = 0x11 (FIN + ACK)
   - Our seq = 1
   - Ack = their seq + 1
2. Compute TCP checksum with pseudo-header
3. Compute IP checksum
4. Wrap in PPP frame → `send_ppp_frame()`

**Real modem errors the sim doesn't produce at this stage (TCP):**

| Error | What happens on real hardware | What driver should do |
|---|---|---|
| TCP RST (flags=0x04) | Remote server forcibly closed the connection | Clean up connection state, report to application |
| SYN timeout (no SYN-ACK) | Server unreachable or port closed | Retry SYN with exponential backoff (typically 3 attempts) |
| Connection reset during data transfer | Server crashed, network path changed | Detect RST, close socket, application retries |
| Window size = 0 | Remote side's receive buffer is full (backpressure) | Stop sending, wait for window update |
| Out-of-order segments | Packets arrived in wrong order (common on real networks) | Buffer and reorder (lwIP handles this) |
| Duplicate ACKs | Packet loss detected by receiver | Trigger fast retransmit (lwIP handles this) |
| TCP checksum mismatch | Corrupted segment | Drop silently, sender will retransmit |
| Half-open connection | One side thinks connection is open, other side doesn't | Detect via keepalive timeout, send RST |

---

### S5.6: Escape Detection (+++)

**High-level:** The driver sends three '+' with guard time (silence before and after) to leave data mode without dropping the PDP. We count '+' in S5.0; when we have three and then a read timeout (silence), we treat that as escape. **What changes:** We set state to PDP_ACTIVE and return from handle_ppp_data_mode(); the main task loop then runs the AT command loop again (S4, then S1-style behavior on next AT).

```
Purpose: Return from DATA_MODE to AT command mode
Runs:    Inline in handle_ppp_data_mode byte loop
```

| Condition | Action | Next State |
|---|---|---|
| byte == '+' | plus_count++ | Stay in S5 |
| plus_count reaches 3, then timeout | Escape confirmed | → S4 (PDP_ACTIVE) |
| plus_count >= 1 but non-'+' byte arrives | Reset plus_count = 0 | Stay in S5 |

On escape:
- `config->state = SIM_MODEM_STATE_PDP_ACTIVE`
- `handle_ppp_data_mode()` returns
- Outer loop in `sim_modem_task` re-enters AT command loop (S4)

**Real modem errors the sim doesn't produce at this stage:**

| Error | What happens on real hardware | What driver should do |
|---|---|---|
| `+++` timing not met | Real modems require 1s silence before AND after `+++` — if data is flowing, the `+` bytes are treated as PPP data, not escape | Ensure guard time (pause TX for 1s, send `+++`, wait 1s for `OK`) |
| Modem responds `NO CARRIER` instead of `OK` | Network dropped during the escape attempt | Handle as disconnect — re-register and re-establish PDP |
| Modem doesn't support `+++` | Some modems use DTR pin drop to exit data mode instead | Check modem datasheet — may need hardware DTR toggle |
| `+++` interpreted as PPP data | If `0x2B 0x2B 0x2B` appears inside a PPP frame payload | Should not happen if guard times are respected, but byte stuffing can cause confusion |

---

## Complete State Diagram

```
                    ┌───────────┐
                    │  S0: OFF  │
                    └─────┬─────┘
                          │ 1.5s delay
                          ▼
                    ┌───────────┐
              ┌────►│ S1: READY │◄──────────────────────────────────┐
              │     └─────┬─────┘                                    │
              │           │ AT+CPIN? → +CPIN: READY                  │
              │           ▼                                          │
              │     ┌─────────────┐                                  │
              │     │S2: SIM_READY│                                  │
              │     └─────┬───────┘                                  │
              │           │ AT+CREG? → +CREG: 0,1                   │
              │           ▼                                          │
              │     ┌──────────────┐                                 │
              │     │S3: REGISTERED│                                 │
              │     └──────┬───────┘                                 │
              │            │ AT+CGACT=1,1 → OK                      │
              │            ▼                                         │
              │     ┌──────────────┐         +++ escape              │
              │     │S4: PDP_ACTIVE│◄────────────────────────┐       │
              │     └──────┬───────┘                         │       │
              │            │ ATD*99# → CONNECT 115200        │       │
              │            ▼                                 │       │
              │     ┌──────────────┐                         │       │
              │     │S5: DATA_MODE │─────────────────────────┘       │
              │     │              │                                  │
              │     │  ┌─────────────────────────────────┐           │
              │     │  │ S5.0: Byte Read Loop            │           │
              │     │  │   ├─ timeout + 3 '+'  → S4 ────┼───────────┘
              │     │  │   ├─ byte == 0x7E     → S5.2    │
              │     │  │   └─ byte != 0x7E     → accum   │
              │     │  │                                  │
              │     │  │ S5.2: Frame Dispatch             │
              │     │  │   ├─ 0xC021 → S5.3 (LCP)       │
              │     │  │   ├─ 0x8021 → S5.4 (IPCP)      │
              │     │  │   └─ 0x0021 → S5.5 (IPv4)      │
              │     │  │       ├─ ICMP echo → reply       │
              │     │  │       ├─ UDP:53    → DNS resp    │
              │     │  │       └─ TCP SYN   → SYN-ACK    │
              │     │  │           TCP FIN  → FIN-ACK    │
              │     │  └─────────────────────────────────┘
              │     └──────────────┘
              │
              │  (outer while(1) in sim_modem_task loops
              │   back to AT command phase after PPP exits)
              └──────────────────────────────────────────
```

---

## Guard Conditions Summary

These prevent out-of-order transitions in the AT command dispatcher:

| Command | Required Minimum State | If Not Met |
|---|---|---|
| `AT+CPIN?` | READY | → ERROR |
| `AT+CREG?` / `AT+CEREG?` | SIM_READY | → +CREG: 0,0 (not registered) |
| `AT+CGACT=1,1` | REGISTERED | → ERROR |
| `ATD*99#` | PDP_ACTIVE (exact) | → NO CARRIER |

---

## Error Paths

| Where | Condition | Response |
|---|---|---|
| AT dispatcher | Unknown command | `"\r\nERROR\r\n"` |
| AT dispatcher | Command valid but state too low | `"\r\nERROR\r\n"` or `"\r\nNO CARRIER\r\n"` |
| LCP handler | frame_len < 8 | Log warning, no response |
| LCP handler | code != 0x01 or invalid length | Log warning, no response |
| IPCP handler | frame_len < 8 | Log warning, no response |
| IPCP handler | unhandled code | Log warning, no response |
| IPv4 handler | ip_len < 20 | Log warning, no response |
| IPv4 handler | unhandled protocol | Log info, no response |
| PPP dispatch | invalid PPP header (not 0xFF/0x03) | Log warning, discard frame |
| PPP dispatch | unknown protocol ID | Log info, discard frame |
| Frame accumulator | frame_len >= 512 (overflow) | Log warning, drop partial frame |
