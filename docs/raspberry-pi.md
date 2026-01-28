# Soluna on Raspberry Pi

This guide covers installing and configuring Soluna on Raspberry Pi.

## Quick Install

```bash
curl -sSL https://soluna.dev/install.sh | sudo bash
```

This will:
1. Detect your Pi model and architecture
2. Install dependencies (libasound2, libssl3)
3. Download and install the Soluna package
4. Detect audio devices
5. Generate default configuration
6. Enable and start the systemd service

## Supported Models

| Model | Status | Notes |
|-------|--------|-------|
| Raspberry Pi 5 | ✅ | Recommended |
| Raspberry Pi 4 | ✅ | Recommended |
| Raspberry Pi 3 | ✅ | Good performance |
| Raspberry Pi Zero 2 W | ✅ | WiFi streaming |
| Raspberry Pi Zero W | ⚠️ | Limited (single core) |

## Manual Installation

### From Package

```bash
# Download package
wget https://github.com/example/soluna/releases/download/v0.1.0/soluna_0.1.0_arm64.deb

# Install
sudo dpkg -i soluna_0.1.0_arm64.deb

# Install missing dependencies if needed
sudo apt-get install -f
```

### From Source

```bash
# Install build dependencies
sudo apt-get install git cmake g++ libasound2-dev libssl-dev

# Clone and build
git clone https://github.com/example/soluna.git
cd soluna
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

## Configuration

Edit `/etc/soluna/config.yaml`:

```yaml
device:
  name: "living-room-pi"
  audio: "hw:sndrpihifiberry"  # HiFiBerry DAC

audio:
  sample_rate: 48000
  channels: 2
```

## Audio Device Setup

### Built-in Audio

```yaml
device:
  audio: "hw:0"
```

### USB Audio

```bash
# List devices
aplay -l

# Use USB device (usually card 1)
device:
  audio: "hw:1"
```

### HiFiBerry DAC

1. Edit `/boot/config.txt`:

```
# Disable onboard audio
dtparam=audio=off

# Enable HiFiBerry
dtoverlay=hifiberry-dacplus
```

2. Reboot and configure:

```yaml
device:
  audio: "hw:sndrpihifiberry"
```

### IQaudio DAC

```
dtoverlay=iqaudio-dacplus
```

## Service Management

```bash
# Check status
sudo systemctl status soluna

# View logs
sudo journalctl -u soluna -f

# Restart
sudo systemctl restart soluna

# Stop
sudo systemctl stop soluna

# Disable autostart
sudo systemctl disable soluna
```

## Performance Tuning

### CPU Governor

For consistent low-latency:

```bash
# Set performance mode
echo "performance" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

Add to `/etc/rc.local` for persistence.

### Real-time Priority

The systemd service sets `Nice=-10` and `LimitRTPRIO=99`. For even lower latency:

```bash
# Edit service
sudo systemctl edit soluna

# Add:
[Service]
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=80
```

### Memory Lock

Already enabled via `LimitMEMLOCK=infinity` in the service file.

## Networking

### Wired (Recommended)

Use Ethernet for lowest latency. Enable jumbo frames if your switch supports it:

```bash
sudo ip link set eth0 mtu 9000
```

### WiFi

Works but with higher latency. Use 5GHz band:

```yaml
audio:
  frames_per_packet: 96  # 2ms for WiFi
  buffer_packets: 16     # More buffering
```

### Static IP

Edit `/etc/dhcpcd.conf`:

```
interface eth0
static ip_address=192.168.1.50/24
static routers=192.168.1.1
static domain_name_servers=192.168.1.1
```

## Monitoring

### Prometheus Metrics

Enabled by default on port 9100:

```bash
curl http://localhost:9100/metrics
```

### Grafana Dashboard

Import the Soluna dashboard from `docs/grafana-dashboard.json`.

## Troubleshooting

### No Audio Output

1. Check ALSA:
   ```bash
   aplay -l
   speaker-test -c 2
   ```

2. Check permissions:
   ```bash
   sudo usermod -a -G audio soluna
   ```

3. Check config:
   ```bash
   cat /etc/soluna/config.yaml
   ```

### High Latency

1. Use Ethernet instead of WiFi
2. Reduce `frames_per_packet`
3. Enable performance CPU governor

### Service Won't Start

```bash
# Check logs
sudo journalctl -u soluna -n 50

# Test manually
sudo -u soluna /usr/bin/solunad --config /etc/soluna/config.yaml
```

### Permission Denied

```bash
# Ensure audio group membership
sudo usermod -a -G audio soluna

# Restart service
sudo systemctl restart soluna
```

## Headless Setup

For running without monitor:

1. Enable SSH:
   ```bash
   sudo raspi-config  # Interface Options > SSH
   ```

2. Find IP:
   ```bash
   hostname -I
   ```

3. Access web UI: `http://<pi-ip>:8400/`

## Uninstall

```bash
curl -sSL https://soluna.dev/uninstall.sh | sudo bash
```

Or manually:

```bash
sudo systemctl stop soluna
sudo systemctl disable soluna
sudo rm /etc/systemd/system/soluna.service
sudo dpkg -r soluna
```
