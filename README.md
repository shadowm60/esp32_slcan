# ESP32 SLCAN adapter

ESP-IDF firmware that connects a CAN bus to a host using the LAWICEL/SLCAN
serial protocol. It supports UART serial and Bluetooth Classic SPP transports.
The Arduino sketch in [`inspiration/esp32-twai-can.ino`](inspiration/esp32-twai-can.ino)
is the reference implementation used to track feature parity; the firmware in
`main/` is the implementation being tested.

> **Porting status:** the feature matrix below describes what the source code
> implements. It does not claim that a feature has been verified on physical
> hardware. Use the checklist to record hardware results.

## Contents

- [Hardware and connections](#hardware-and-connections)
- [Build and flash](#build-and-flash)
- [Using the adapter](#using-the-adapter)
- [SLCAN command reference](#slcan-command-reference)
- [Arduino-to-ESP-IDF feature matrix](#arduino-to-esp-idf-feature-matrix)
- [Hardware test checklist](#hardware-test-checklist)
- [Known gaps and differences](#known-gaps-and-differences)

## Hardware and connections

The current ESP-IDF project configuration targets the classic ESP32
(`CONFIG_IDF_TARGET="esp32"`). The Arduino reference sketch comments describe
an ESP32-S3 setup, so do not assume the two builds have identical target
support.

| Signal | ESP32 GPIO | Connection |
| --- | ---: | --- |
| TWAI/CAN TX | 4 | CAN transceiver TX input |
| TWAI/CAN RX | 5 | CAN transceiver RX output |
| Transport select | 12 | Low selects Bluetooth; high selects UART |
| Ground | GND | Common ground with the transceiver and CAN peer |

Use a CAN transceiver between the ESP32's 3.3 V logic and CANH/CANL. Do not
connect the ESP32 pins directly to CANH/CANL. Terminate the bus appropriately
and use a second active CAN node when testing transmit/receive.

GPIO 12 is configured with an internal pull-up. Leaving the switch open (or
driving it high) selects UART; grounding it at boot selects Bluetooth Classic
SPP. Choose the transport before resetting the board.

GPIO 12 is also a strapping pin on the classic ESP32. Check the flash-voltage
strapping requirements for the specific module/board before wiring a switch
that drives this pin during reset; the board must still boot reliably in both
switch positions.

The SLCAN UART is UART0 at **921600 baud** by default
(`CONFIG_ESP32_SLCAN_UART_BAUDRATE`, set in menuconfig under "ESP32 SLCAN
Bluetooth Configuration"); the Arduino sketch used 500000 baud. The bootloader
and early startup logs still go out at `CONFIG_ESP_CONSOLE_UART_BAUDRATE`
(115200); once the UART transport starts, the firmware switches baud rate and
disables log output so only SLCAN traffic is sent. Host tools should flush
their receive buffer after opening the port. Startup checks that UART0's driver and baud-rate setup
succeed. In Bluetooth mode startup waits up to 10 seconds for the SPP server
to start and become connectable/discoverable; it does not wait for a host to
pair or connect. If the selected transport fails initialization or readiness,
the firmware logs the error and does not start the SLCAN/CAN tasks.

## Build and flash

Use an ESP-IDF environment with the target and required toolchain installed.
From the project directory:

```text
idf.py set-target esp32
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with the board's serial port. To change Bluetooth's advertised
name or Secure Simple Pairing configuration, use `idf.py menuconfig` under
**ESP32 SLCAN Bluetooth Configuration**. The default device name is
`ESP32_SLCAN`.

The serial monitor shares UART0 with the SLCAN transport. For command testing,
use a terminal or host SLCAN client that can send a carriage return (`CR`,
byte `0x0D`) without adding other characters.

## Using the adapter

1. Connect the board to a CAN transceiver and a properly terminated CAN bus.
2. Select the transport with GPIO 12 and reset the board.
3. Open the selected UART or Bluetooth SPP connection. For UART, use 921600
   baud (or your configured `CONFIG_ESP32_SLCAN_UART_BAUDRATE`), 8 data bits, no parity, and 1 stop bit.
4. Send SLCAN commands terminated by `CR`. Line feed (`LF`) bytes are ignored.
5. Select the CAN bitrate with `S0` through `S8` **before** opening the CAN
   interface with `O` or `L`.
6. Close the CAN interface with `C`.

Successful commands return a carriage return (`CR`, `0x0D`). Rejected commands
return a bell (`BEL`, `0x07`). `V`, `v`, `N`, and `F` return a value string
terminated by `CR`.

## SLCAN command reference

| Command | Meaning | Response / notes |
| --- | --- | --- |
| `O` | Open CAN in normal mode | `CR` on success; `BEL` on failure |
| `L` | Open CAN in listen-only mode | ESP-IDF addition; transmit commands are rejected while listening |
| `C` | Close CAN | `CR` on success; `BEL` if closing fails |
| `S0`–`S8` | Select bitrate | Must be sent while CAN is closed; see bitrate table below |
| `t` / `T` | Transmit standard / extended data frame | Send command in the frame format below |
| `r` / `R` | Transmit standard / extended remote frame (RTR) | No data bytes follow the DLC |
| `Z0` / `Z1` | Disable / enable receive timestamps | Timestamp is appended to received frame strings |
| `F` | Read status flags | Returns `Fxx` followed by `CR` |
| `V` / `v` | Read firmware version | Returns `V0100` / `v0100` followed by `CR` |
| `N` | Read serial number | Returns the last three Bluetooth device-address bytes as six hex digits |

### Bitrates

| Command | Bitrate |
| --- | ---: |
| `S0` | 10 kbit/s |
| `S1` | 20 kbit/s |
| `S2` | 50 kbit/s |
| `S3` | 100 kbit/s |
| `S4` | 125 kbit/s |
| `S5` | 250 kbit/s |
| `S6` | 500 kbit/s |
| `S7` | 800 kbit/s |
| `S8` | 1 Mbit/s |

### Frame command format

Commands use hexadecimal digits. Standard identifiers have 3 digits;
extended identifiers have 8 digits. DLC is one hex digit (0–8). Data frames
include two hex digits per data byte:

```text
t{3-digit-id}{dlc}{data...}       standard data frame
T{8-digit-id}{dlc}{data...}       extended data frame
r{3-digit-id}{dlc}                standard RTR frame
R{8-digit-id}{dlc}                extended RTR frame
```

For example, `t1232DEAD` transmits a standard frame with ID `0x123`, DLC 2,
and data bytes `DE AD`. The command itself must be followed by `CR`.

Received frames use the same format and end in `CR`. With timestamps enabled,
four hexadecimal timestamp digits are appended before the `CR`.

### Status flags

`F` returns the current flags as two hexadecimal digits. The current
implementation reports:

| Bit | Meaning |
| ---: | --- |
| 2 | CAN error warning |
| 3 | Receive queue overflow since the last `F` query |
| 4 | CAN error passive |
| 7 | CAN bus-off |

The receive-overflow bit is cleared when read. Other bits are derived from the
TWAI driver's current state.

## Arduino-to-ESP-IDF feature matrix

Status definitions:

- **Implemented (code):** an implementation is present; hardware behavior
  still needs to be checked with the checklist below.
- **Partial/different:** related behavior exists, but differs from the Arduino
  reference or needs a compatibility decision.
- **Missing:** the ESP-IDF firmware does not implement the feature.

| Feature | Arduino reference | ESP-IDF status | Verification / notes |
| --- | --- | --- | --- |
| Open and close CAN (`O`, `C`) | Normal mode open/close | **Implemented (code)** | Hardware test pending |
| Listen-only open (`L`) | Not present | **Implemented (code)** | ESP-IDF-only feature; test that transmit is rejected |
| Standard and extended CAN data frames | Transmit and receive | **Implemented (code)** | Verify both directions with a second CAN node |
| Standard and extended RTR frames | Transmit and receive | **Implemented (code)** | Verify both directions; RTR has no data bytes |
| Standard bitrates (`S0`–`S8`) | Present; `S0`/`S1` only advertised for S3 | **Implemented (code)** | Test on the configured ESP32 target at each bitrate used |
| Custom bitrate (`s`) | Always NACKs; not implemented | **Missing** | No custom bitrate command in ESP-IDF firmware |
| Timestamps (`Z0`, `Z1`) | Present; uses `millis() % 60000` | **Partial/different** | ESP-IDF appends four hex digits from a 16-bit millisecond tick value; verify wrap/range expectations |
| Acceptance code/mask (`M`, `m`) | ACKs but does not apply filters | **Not implemented (reference stub)** | Arduino behavior is a no-op; ESP-IDF does not accept these commands |
| Status flags (`F`) | Always returns `F00` | **Partial/different** | ESP-IDF reports warning/passive/bus-off/queue-overflow state; test error and overflow cases |
| Version (`V`) | Static `V1234` | **Partial/different** | ESP-IDF returns static `V0100` |
| Lowercase version (`v`) | Not present | **Implemented (code)** | ESP-IDF addition; returns `v0100` |
| Serial number (`N`) | Static `N2233` | **Partial/different** | ESP-IDF derives it from the Bluetooth address; test in both transport modes |
| USB/UART SLCAN transport | Serial at 500000 baud | **Implemented (code)** | ESP-IDF uses UART0 at configurable baud (default 921600) |
| Bluetooth Classic SPP transport | Explicitly not included | **Implemented (code)** | GPIO 12 selects transport; pair/connect and exercise commands |
| CR-terminated commands, optional LF | Reads through CR | **Implemented (code)** | ESP-IDF accepts fragmented input and ignores LF; input accumulation resets when the 63-character buffer fills |
| Help command (`h`) | Prints a human-readable help page | **Missing** | Non-standard command; no ESP-IDF help response |
| Newline toggle (`l`) | Toggles an extra LF after received frames | **Missing** | Non-standard command; ESP-IDF always uses CR frame terminators |
| Automated protocol tests | None in the Arduino sketch | **Missing** | Current verification procedure is manual; record results in checklist below |

## Hardware test checklist

Mark a check only after observing the expected behavior on the target hardware.
Record the board revision, ESP-IDF version, transceiver, peer device, bitrate,
and date alongside the results when using this checklist for a release.

### Setup and transport

- [ ] Build and flash the firmware for the intended ESP32 target.
- [ ] UART mode: leave GPIO 12 high/open, reset, connect at 921600 8N1, and
  confirm SLCAN commands receive responses.
- [ ] Bluetooth mode: hold GPIO 12 low during reset, pair/connect to
  `ESP32_SLCAN`, and confirm SLCAN commands receive responses.
- [ ] Send `V`, `v`, `N`, and `F`; check for `V0100`, `v0100`, `N` plus six hex
  digits, and `F` plus two hex digits respectively, each terminated by `CR`.
- [ ] Send an unsupported command and confirm the response is `BEL`.

### CAN operation

- [ ] For each bitrate required by the product, send its `S` command while
  closed and confirm `CR`; confirm a bitrate change while open returns `BEL`.
- [ ] Send `O` and `C`; confirm each succeeds and the CAN interface follows the
  expected open/closed state.
- [ ] With a second active CAN node at the same bitrate, receive standard
  frame `t1232DEAD`; expect `t1232DEAD` from the adapter with timestamps off.
- [ ] Receive and verify an extended data frame (for example
  `T000001232DEAD` for ID `0x123`, DLC 2, data `DE AD`).
- [ ] Send `t1232DEAD` through the adapter and verify the peer receives ID
  `0x123`, DLC 2, and bytes `DE AD`.
- [ ] Repeat transmit and receive checks for standard and extended RTR frames.
- [ ] Open with `L`, verify received frames are reported, and confirm a
  transmit command is rejected with `BEL`.
- [ ] Send `Z1`, verify a received frame has four timestamp hex digits appended,
  then send `Z0` and verify the suffix is absent.
- [ ] Exercise status reporting (`F`) during normal operation and, if possible,
  test error warning, error passive, bus-off, and receive queue overflow flags.

### Result log

| Date | Board / target | ESP-IDF version | Tests passed / total | Notes |
| --- | --- | --- | --- | --- |
|  |  |  |  |  |

## Known gaps and differences

- The Arduino sketch is a reference, not an automated test oracle. Some of its
  commands (`M`, `m`) only acknowledge requests and do not implement their
  advertised feature.
- The ESP-IDF firmware has not been declared hardware-verified by this
  document. Update the matrix and checklist with observed results as tests are
  completed; do not mark a feature as verified based on compilation alone.
- The project currently has no automated SLCAN protocol test suite. A future
  host-side test should exercise command parsing, ACK/NACK behavior, frame
  encoding, and transport framing without requiring a physical CAN bus.
- `N` uses the Bluetooth device address, so its availability/value over UART
  should be confirmed on the target.
- The Arduino and ESP-IDF implementations report different version and serial
  values and use different timestamp calculations and UART baud rates.
