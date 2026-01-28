# Installation Guide

This guide covers installing Soluna on various platforms.

## Quick Install

### Linux (Debian/Ubuntu/Raspberry Pi)

```bash
curl -sSL https://soluna.dev/install.sh | sudo bash
```

This will:
1. Detect your system architecture (x64, ARM64, ARMv7)
2. Install dependencies (libasound2, libssl3)
3. Download and install the Soluna package
4. Configure the systemd service
5. Start the daemon

### macOS (Homebrew)

```bash
brew tap soluna/tap
brew install soluna
```

### Windows

Download the installer from [Releases](https://github.com/example/soluna/releases):

1. Run `soluna-0.1.0-win64.msi`
2. Follow the installation wizard
3. Launch "Soluna" from Start Menu

## Building from Source

### Prerequisites

**Linux:**
```bash
sudo apt-get install git cmake g++ libasound2-dev libssl-dev
```

**macOS:**
```bash
brew install cmake
# Xcode Command Line Tools required
xcode-select --install
```

**Windows:**
- Visual Studio 2019 or later
- CMake 3.16+
- Git

### Build Steps

```bash
# Clone repository
git clone https://github.com/example/soluna.git
cd soluna

# Create build directory
mkdir build && cd build

# Configure (Release build)
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
make -j$(nproc)  # Linux/macOS
# or
cmake --build . --config Release  # Windows

# Install (optional)
sudo make install  # Linux/macOS
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `SOLUNA_BUILD_TESTS` | ON | Build unit tests |
| `SOLUNA_ENABLE_OPUS` | OFF | Enable Opus codec support |
| `SOLUNA_ENABLE_AES67` | OFF | Enable AES67 compatibility mode |
| `SOLUNA_ENABLE_DTLS` | OFF | Enable DTLS encryption |
| `SOLUNA_ENABLE_AAC` | OFF | Enable AAC codec (requires fdk-aac) |
| `SOLUNA_ENABLE_FLAC` | OFF | Enable FLAC codec (requires libflac) |

Example with options:
```bash
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DSOLUNA_ENABLE_DTLS=ON \
         -DSOLUNA_ENABLE_OPUS=ON
```

### Running Tests

```bash
cd build
ctest --output-on-failure
```

## Platform-Specific Notes

### Linux

**Audio Group:**
Add your user to the `audio` group for device access:
```bash
sudo usermod -a -G audio $USER
# Log out and back in for changes to take effect
```

**Real-time Priority:**
For lowest latency, enable real-time scheduling:
```bash
# Edit /etc/security/limits.conf
@audio - rtprio 99
@audio - memlock unlimited
```

### macOS

**Microphone Permission:**
For input capture, grant microphone access:
System Preferences → Security & Privacy → Privacy → Microphone → Enable for Terminal/your app

**Audio Device:**
Use `default` or list devices with:
```bash
solunad --list-devices
```

### Windows

**Firewall:**
Allow Soluna through Windows Firewall:
- Settings → Update & Security → Windows Security → Firewall
- Allow an app through firewall → Add `solunad.exe`

**Audio Device:**
Uses WASAPI shared mode by default. For exclusive mode, edit config.

## Verifying Installation

```bash
# Check version
solunad --version

# List audio devices
solunad --list-devices

# Test transmission
solunad --tx --device default --dest 239.69.0.1:5004

# Test reception (in another terminal)
solunad --rx --device default --port 5004
```

## Upgrading

### Linux

```bash
# Using installer script
curl -sSL https://soluna.dev/install.sh | sudo bash

# Using apt (if repository configured)
sudo apt update && sudo apt upgrade soluna
```

### macOS

```bash
brew upgrade soluna
```

### Windows

Download and run the latest installer. It will upgrade the existing installation.

## Uninstalling

### Linux

```bash
# Using uninstall script
curl -sSL https://soluna.dev/uninstall.sh | sudo bash

# Manual
sudo systemctl stop soluna
sudo systemctl disable soluna
sudo dpkg -r soluna
sudo rm -rf /etc/soluna
```

### macOS

```bash
brew uninstall soluna
```

### Windows

Settings → Apps → Soluna → Uninstall

## Next Steps

- [Configuration Reference](configuration.md) - Configure Soluna for your needs
- [API Reference](api.md) - Control Soluna programmatically
- [Troubleshooting](troubleshooting.md) - Solve common issues
