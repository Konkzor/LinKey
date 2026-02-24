# Linkey

Low-power ESP32 firmware for monitoring French Linky electricity meters using the ULP coprocessor with light sleep mode.

## Overview

This firmware uses the ESP32's Ultra Low Power (ULP) coprocessor to continuously monitor the Linky meter's TIC (Télé-Information Client) serial output while the main CPU is in light sleep. The ULP collects multiple lines, then wakes the main CPU to publish data via MQTT.

### Features

- **True 7E1 UART support**: Properly handles 7 data bits + even parity (skipped)
- **Light sleep mode**: WiFi stays connected, fast wake-up (TBD)
- **WiFi optimizations**:
  - BSSID/channel caching for instant reconnection
  - Fast scan mode
  - WiFi modem sleep for power saving
  - Optional static IP (skips DHCP)
- **MQTT optimizations**:
  - QoS 1 for reliable delivery and better wifi disconnection detection
  - Fast timeouts
  - Last Will and Testament (LWT) for availability tracking
- **Home Assistant integration**:
  - MQTT device auto-discovery (single payload)
  - Availability monitoring (online/offline)
  - Custom icons and state classes for proper HA statistics
- **Monitored values**:
  - `IINST`: Instantaneous current (Amperes)
  - `BASE`: Energy index (Watt-hours)
  - `PAPP`: Apparent power (VA)
  - `ADPS`: Overcurrent warning current (Amperes)
  - `VCAP`: Supercap voltage (millivolts)
  - `uptime`: Device uptime (seconds)
- **Multi-line buffering**: ULP collects 10 lines before waking CPU
- **Checksum validation**: Validates Linky TIC checksum `(sum & 0x3F) + 0x20`
- **RGB LED status**: Visual feedback for voltage and operation status
- **Supercap voltage monitoring**: ADC reading on GPIO 33
- **Debug logging**: Configurable verbose logging for troubleshooting

## Hardware Requirements

- **ESP32** (original, not ESP32-C3/S2/S3)
- **Linky meter** with TIC output
- **Optocoupler circuit** (required for galvanic isolation)
- **Supercapacitor** for energy storage
- **RGB LED** (optional, for status indication)

### GPIO Assignments

| Function | GPIO |
|----------|------|
| Linky RX | 14 (configurable) |
| Supercap ADC | 33 |
| RGB Red LED | 13 |
| RGB Green LED | 15 |
| RGB Blue LED | 2 |

### Linky TIC Connection (with Optocoupler)

The Linky meter provides a TIC output that **requires galvanic isolation** using an optocoupler.

**Note**: The optocoupler inverts the signal. The Linky TIC output is designed to work with this configuration.

## Software Requirements

- **ESP-IDF v5.4.1** or later
- **HULP library** (included as submodule)

## Building and Flashing

### 1. Clone and Initialize

```bash
cd /path/to/LinKey/Firmware
git submodule update --init --recursive
```

### 2. Set up ESP-IDF environment

```bash
source ~/esp/v5.4.1/esp-idf/export.sh
```

### 3. Configure the project

```bash
idf.py menuconfig
```

Navigate to **"Linkey Monitor Configuration"** and configure:

#### Required Settings:
- **Device Name**: Name shown in Home Assistant (default: `Linkey`)
- **WiFi SSID**: Your WiFi network name
- **WiFi Password**: Your WiFi password
- **MQTT Broker URI**: e.g., `mqtt://192.168.1.100`
- **MQTT Topic Prefix**: Default `linkey` (topics: `linkey/state`, `linkey/status`)
- **Linky RX GPIO**: Default `14` (must be RTC-capable GPIO)

#### Optional Settings:
- **MQTT Username/Password**: If your broker requires authentication
- **Use Static IP**: Enable for faster initial connection
- **Enable debug logging**: For troubleshooting

### 4. Build

```bash
idf.py build
```

### 5. Flash

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

## Operation

### Boot Sequence

1. **Initialize peripherals**:
   - RGB LEDs
   - Supercap ADC
   - Power management (light sleep enabled)

2. **Wait for supercap charge**:
   - Waits until supercap voltage > 2.5V
   - Red LED blinks if < 2V
   - Orange (red+green) if 2-2.5V
   - Green flash when ready

3. **Initialize connectivity**:
   - Connect to WiFi (with modem sleep)
   - Connect to MQTT broker
   - Initialize ULP program

### Main Loop

1. **Light sleep**:
   - CPU enters automatic light sleep
   - WiFi stays connected (modem sleep)
   - ULP continuously receives UART data

2. **ULP operation**:
   - Bit-bangs UART at 1200 baud (7E1)
   - Collects 10 lines into buffers
   - Wakes main CPU periodically

3. **Main CPU wake-up**:
   - Reads supercap voltage (LED feedback)
   - Reads stored values from ULP buffers
   - Validates checksums
   - Publishes valid data to MQTT
   - Returns to light sleep
   - **Wake time**: TBD

### MQTT Topics

With default prefix `linkey`:
- `linkey/state` - JSON payload with sensor data (only valid values included):
  ```json
  {"iinst":3,"base":12345678,"papp":690,"vcap":2850,"uptime":3600}
  ```
- `linkey/status` - Device availability (`online`/`offline` via LWT)

**Note**: Only Linky values with valid checksums are included in the JSON. VCAP and uptime are always present.

### Home Assistant

The device is auto-discovered via MQTT. On connection, a single discovery payload is published to:
```
homeassistant/device/linkey_<mac>/config
```

This registers the device with all sensors in Home Assistant. The device appears with:
- **Current** (IINST) - instantaneous measurement
- **Energy Index** (BASE) - total increasing for energy dashboard
- **Apparent Power** (PAPP) - instantaneous measurement
- **Overcurrent Warning** (ADPS) - overcurrent alert
- **Supercap Voltage** (VCAP) - instantaneous measurement
- **Uptime** - device uptime in seconds

### LED Status Indicators

| Voltage | LED Color |
|---------|-----------|
| < 1V | Red blink |
| 1-2V | Orange blink |
| > 2V | Green blink |

## Linky TIC Message Format

The Linky meter sends messages in this format:
```
<LABEL> <DATA> <CHECKSUM>\r\n
```

Example:
```
IINST 003 :
BASE 012345678 '
PAPP 00690 +
ADPS 030 !
```

**Serial parameters**: 1200 baud, 7 data bits, Even parity, 1 stop bit (7E1)

**Checksum calculation**:
```c
uint8_t checksum = (sum_of_bytes & 0x3F) + 0x20;
```

## Power Consumption

- **Light sleep**: TBD
- **Active (publishing)**: TBD
- **Average**: TBD

## Project Structure

```
Firmware/
├── CMakeLists.txt              # Main project CMake
├── sdkconfig.defaults          # Default configuration
├── main/
│   ├── CMakeLists.txt          # Component CMake
│   ├── Kconfig.projbuild       # Configuration menu
│   ├── main.c                  # FSM application (state machine, voltage monitoring)
│   ├── ulp_linky.cpp           # ULP program (7E1 UART, TIC parsing)
│   ├── ulp_linky.h             # ULP program header
│   ├── wifi_manager.c          # WiFi connection management
│   ├── wifi_manager.h          # WiFi manager header
│   ├── mqtt_manager.c          # MQTT client, HA discovery, JSON publishing
│   ├── mqtt_manager.h          # MQTT manager header
│   └── debug.h                 # Debug logging macros
└── HULP/                       # HULP library (submodule)
```

## Architecture

### FSM States

```
INIT → WAIT_VOLTAGE → WIFI_CONNECT → MQTT_CONNECT → WAIT_ULP_DATA → PUBLISH_DATA
                ↑                                                         │
                └─────────────── voltage low ──────────────────────────────┘
```

Any state falls back to `WAIT_VOLTAGE` if supercap voltage drops below threshold.

### System Overview

```
┌─────────────────────────────────────────────┐
│  Main CPU (FSM) (Periodic wake)             │
│  • Voltage monitoring with per-state        │
│    fallback thresholds                      │
│  • WiFi/MQTT connection management          │
│  • HA auto-discovery on MQTT connect        │
│  • JSON sensor data publishing              │
│  • Modem sleep + CPU frequency scaling      │
└─────────────────────────────────────────────┘
                    ▲
                    │
                    │
┌─────────────────────────────────────────────┐
│  ULP Coprocessor (Always Running)           │
│  • Bit-bangs UART at 1200 baud (7E1)        │
│  • Collects 10 lines into buffers           │
│  • Runs continuously                        │
└─────────────────────────────────────────────┘
```

## References

- [HULP Library](https://github.com/boarchuz/HULP)
- [ESP32 ULP Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/ulp.html)
- [Linky TIC Documentation](Doc/Enedis-MOP-CPT_002E.pdf) - Linky TIC specification (included in `Doc/`)
- [ESP32 Power Management](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/power_management.html)
- [HA MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery)

## License

This project uses the HULP library which has its own license. Check the HULP directory for details.
