# ESP32 Firmware Guide

This guide covers building, flashing, and configuring Soluna on ESP32 microcontrollers.

## Supported Hardware

| Board | Status | Notes |
|-------|--------|-------|
| ESP32-DevKitC | ✅ | Recommended for beginners |
| ESP32-S3-DevKitC | ✅ | Best performance (dual-core, PSRAM) |
| ESP32-WROVER | ✅ | 8MB PSRAM for buffering |
| ESP32-C3 | ⚠️ | Single core, limited performance |
| ESP32-S2 | ⚠️ | Single core, no Bluetooth |

## Hardware Setup

### Wiring Diagram (I2S DAC)

```
ESP32          PCM5102A DAC
------         ------------
GPIO25  -----> BCLK (Bit Clock)
GPIO26  -----> LRCK (Word Select)
GPIO22  -----> DIN  (Data In)
3.3V    -----> VCC
GND     -----> GND
GND     -----> SCK (tie to ground)
```

### Budget Build (~$15)

| Component | Purpose | Link |
|-----------|---------|------|
| ESP32-DevKitC | MCU | [AliExpress](https://aliexpress.com/item/1005001267643044.html) |
| PCM5102A Module | DAC | [AliExpress](https://aliexpress.com/item/32836612292.html) |
| Dupont Wires | Connection | Any |
| USB Cable | Power + Programming | Any |

### Standard Build (~$25)

| Component | Purpose | Link |
|-----------|---------|------|
| ESP32-S3-DevKitC | MCU (better performance) | [AliExpress](https://aliexpress.com/item/1005004452396855.html) |
| PCM5102A Module | DAC | [AliExpress](https://aliexpress.com/item/32836612292.html) |
| LM386 Amplifier | Amp (optional) | [AliExpress](https://aliexpress.com/item/32833637279.html) |
| 3D Printed Case | Enclosure | [Printables](https://printables.com) |

### Premium Build (~$60)

| Component | Purpose | Link |
|-----------|---------|------|
| ESP32-S3-WROOM-1 | MCU | [Mouser](https://mouser.com) |
| ES9038Q2M DAC | Hi-Fi DAC | [AliExpress](https://aliexpress.com/item/1005003493783197.html) |
| TPA3116D2 Amp | Class-D Amp | [AliExpress](https://aliexpress.com/item/1005003108391596.html) |
| Aluminum Case | EMI Shielding | Amazon |

## Building Firmware

### Prerequisites

Install ESP-IDF v5.0+:

```bash
# Linux/macOS
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh
source export.sh

# Windows: Use ESP-IDF Tools Installer
```

### Build Steps

```bash
# Navigate to ESP32 firmware
cd apps/esp32

# Set target board
idf.py set-target esp32  # or esp32s3

# Configure (optional)
idf.py menuconfig

# Build
idf.py build

# Flash (replace /dev/ttyUSB0 with your port)
idf.py -p /dev/ttyUSB0 flash

# Monitor serial output
idf.py -p /dev/ttyUSB0 monitor
```

### Build Configuration

In `idf.py menuconfig`:

```
Soluna Configuration --->
    [*] Enable WiFi
    [ ] Enable Bluetooth Audio (A2DP)
    [*] Enable FEC (Forward Error Correction)
    (2)  Number of audio channels
    (48000) Sample rate

Component config --->
    ESP32-specific --->
        (240) CPU frequency
    Wi-Fi --->
        [*] WiFi AMPDU TX
        [*] WiFi AMPDU RX
```

## Flashing Pre-built Firmware

### Using esptool.py

```bash
# Install esptool
pip install esptool

# Download firmware
wget https://github.com/example/soluna/releases/download/v0.1.0/soluna-esp32.bin

# Flash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
    write_flash -z 0x10000 soluna-esp32.bin
```

### Using Web Flasher

Visit [https://soluna.dev/flash](https://soluna.dev/flash) and follow instructions.
(Requires Chrome/Edge with Web Serial API)

## Initial Configuration

### Method 1: Web UI (Recommended)

1. Power on ESP32
2. Connect to WiFi AP: `Soluna-XXXXXX` (password: `soluna123`)
3. Open browser to `http://192.168.4.1`
4. Configure WiFi and device settings
5. Save and reboot

### Method 2: Serial Console

```bash
# Connect via serial monitor
idf.py -p /dev/ttyUSB0 monitor

# Or use screen
screen /dev/ttyUSB0 115200
```

Commands:
```
help                    - Show available commands
wifi <ssid> <password>  - Set WiFi credentials
name <device-name>      - Set device name
mode <tx|rx|txrx>       - Set operating mode
channel <1-8>           - Set number of channels
save                    - Save configuration to NVS
reboot                  - Restart device
status                  - Show current status
```

### Method 3: NVS Partition

Pre-configure NVS before flashing:

```bash
# Create nvs_data.csv
namespace,key,type,value
soluna,wifi_ssid,string,MyWiFi
soluna,wifi_pass,string,MyPassword
soluna,device_name,string,living-room
soluna,mode,u8,1
soluna,channels,u8,2

# Generate binary
python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
    generate nvs_data.csv nvs.bin 0x6000

# Flash NVS partition
esptool.py write_flash 0x9000 nvs.bin
```

## Configuration Reference

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `wifi_ssid` | string | "" | WiFi network name |
| `wifi_pass` | string | "" | WiFi password |
| `device_name` | string | "esp32-soluna" | Device name for discovery |
| `mode` | u8 | 1 (RX) | 0=TX, 1=RX, 2=TXRX |
| `channels` | u8 | 2 | Audio channels (1-8) |
| `rtp_port` | u16 | 5004 | RTP receive port |
| `fec_enabled` | u8 | 1 | Enable FEC (0/1) |
| `target_latency_ms` | float | 20.0 | Target playback latency |

## Operating Modes

### RX Mode (Receiver)

Receives audio from network and outputs to I2S DAC.

```
Network → RTP/OSTP → Jitter Buffer → I2S DAC → Speakers
```

Use case: Wireless speakers, headphones, PA systems

### TX Mode (Transmitter)

Captures audio from I2S ADC and transmits to network.

```
Microphone → I2S ADC → RTP/OSTP → Network
```

Use case: Wireless microphones, instruments

### TXRX Mode (Bidirectional)

Both TX and RX simultaneously. Useful for intercom systems.

## P2P Standalone Mode

ESP32 can operate without a desktop daemon for direct device-to-device streaming.

### Scenario: Two ESP32s

**Transmitter (with mic):**
- Mode: TX
- Becomes PTP leader automatically
- Announces presence via multicast

**Receiver (with speaker):**
- Mode: RX
- Discovers transmitter automatically
- Syncs to PTP leader

### Scenario: One TX, Multiple RX

**Transmitter:**
- Sends to multicast address
- Acts as PTP grandmaster

**Receivers (multiple):**
- Join multicast group
- All sync to same PTP clock
- Synchronized playback

### Configuration for P2P

On TX device:
```
mode tx
name studio-mic
channel 1
save
reboot
```

On RX device:
```
mode rx
name kitchen-speaker
channel 2
save
reboot
```

## Web Interface

Access at `http://<esp32-ip>/` after connecting to WiFi.

### Dashboard

- Device status (name, mode, IP)
- PTP sync status and offset
- Packet statistics (TX/RX/lost/recovered)
- Audio buffer levels
- Free heap memory

### Settings

- WiFi credentials
- Device name
- Operating mode
- Channel count
- FEC enable/disable
- Target latency

### Actions

- Save configuration
- Reboot device
- Factory reset
- OTA firmware update

## OTA Updates

### Via Web UI

1. Download new firmware `.bin` file
2. Open web interface
3. Go to Settings → Firmware Update
4. Select file and upload
5. Wait for reboot

### Via Command Line

```bash
# Using espota.py
python $IDF_PATH/components/esptool_py/esptool/espota.py \
    -i 192.168.1.50 -p 3232 \
    -f build/soluna.bin
```

### Via HTTP

```bash
curl -X POST http://192.168.1.50/api/ota \
    -F "firmware=@build/soluna.bin"
```

## Troubleshooting

### No Audio Output

1. Check I2S wiring (BCLK, LRCK, DIN)
2. Verify DAC is receiving power
3. Check serial log for errors:
   ```
   idf.py monitor
   ```
4. Verify network connectivity:
   ```
   status
   ```

### WiFi Won't Connect

1. Check credentials (case-sensitive)
2. Ensure 2.4GHz network (ESP32 doesn't support 5GHz)
3. Check router allows new connections
4. Try factory reset:
   ```
   factory_reset
   reboot
   ```

### Audio Glitches/Dropouts

1. Check WiFi signal strength
2. Increase target latency:
   ```
   latency 30
   save
   ```
3. Enable FEC:
   ```
   fec 1
   save
   ```
4. Move closer to router or use Ethernet (ESP32-PoE)

### PTP Not Syncing

1. Check multicast enabled on router
2. Verify all devices on same subnet
3. Check for firewall blocking UDP 319/320
4. View PTP status:
   ```
   ptp_status
   ```

### Device Not Discovered

1. Verify mDNS enabled on network
2. Check multicast routing
3. Ensure devices on same VLAN
4. Try manual connection:
   ```bash
   # On desktop
   solunad --connect 192.168.1.50
   ```

## Performance Tuning

### For Lowest Latency

```
fec 0             # Disable FEC
latency 10        # 10ms target
channel 1         # Mono only
save
```

### For Reliability (WiFi)

```
fec 1             # Enable FEC
latency 30        # 30ms buffer
save
```

### Memory Optimization

```
channel 2         # Limit channels
# Use ESP32-WROVER with PSRAM for more channels
```

## Specifications

| Parameter | Value |
|-----------|-------|
| Sample Rate | 48000 Hz |
| Bit Depth | 24-bit |
| Max Channels | 8 (with PSRAM) |
| Latency | 10-50ms (configurable) |
| Power | 5V via USB, ~300mA typical |
| WiFi | 2.4GHz 802.11b/g/n |
