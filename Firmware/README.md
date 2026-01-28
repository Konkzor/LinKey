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
  - QoS 0 for maximum speed
  - Minimal buffers
  - Fast timeouts
- **Monitored values**:
  - `IINST`: Instantaneous current (Amperes)
  - `BASE`: Energy index (Watt-hours)
  - `VCAP`: Supercap voltage (millivolts)
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

Navigate to **"Linky Monitor Configuration"** and configure:

#### Required Settings:
- **WiFi SSID**: Your WiFi network name
- **WiFi Password**: Your WiFi password
- **MQTT Broker URI**: e.g., `mqtt://192.168.1.100`
- **MQTT Topic Prefix**: Default `linky` (topics will be `linky/iinst`, `linky/base`, `linky/vcap`)
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

With default prefix `linky`:
- `linky/iinst` - Instantaneous current in Amperes
- `linky/base` - Total energy in Watt-hours
- `linky/vcap` - Supercap voltage in millivolts

**Note**: Only values with valid checksums are published.

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
│   ├── main.c                  # Main application (WiFi, MQTT, light sleep)
│   ├── ulp_linky.cpp           # ULP program (7E1 UART, parsing)
│   ├── ulp_linky.h             # ULP program header
│   └── debug.h                 # Debug logging macro
└── HULP/                       # HULP library (submodule)
```

## Architecture

```
┌─────────────────────────────────────────────┐
│  Main CPU (Light Sleep)                     │
│  • WiFi/MQTT stay connected                 │
│  • Wakes periodically                       │
│  • Reads supercap voltage                   │
│  • Publishes valid data                     │
│  • Returns to light sleep                   │
│  • Wake time: TBD                           │
│  • Modem sleep + CPU frequency scaling      │
└─────────────────────────────────────────────┘
                    ▲
                    │ Periodic wake
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
- [Linky TIC Documentation](https://www.enedis.fr/media/2035/download)
- [ESP32 Power Management](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/power_management.html)

## License

This project uses the HULP library which has its own license. Check the HULP directory for details.
