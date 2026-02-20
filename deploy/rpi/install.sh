#!/bin/bash
#
# Soluna Raspberry Pi Installer
#
# One-line install:
#   curl -sSL https://soluna.dev/install.sh | sudo bash
#
# SPDX-License-Identifier: MIT
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

SOLUNA_VERSION="${SOLUNA_VERSION:-0.1.0}"
SOLUNA_USER="soluna"
SOLUNA_GROUP="audio"
CONFIG_DIR="/etc/soluna"
LOG_DIR="/var/log/soluna"
DATA_DIR="/var/lib/soluna"

# Logging
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_step() {
    echo -e "${BLUE}==>${NC} $1"
}

# Check if running as root
check_root() {
    if [[ $EUID -ne 0 ]]; then
        log_error "This script must be run as root (use sudo)"
        exit 1
    fi
}

# Detect Raspberry Pi model
detect_pi_model() {
    if [[ -f /proc/device-tree/model ]]; then
        PI_MODEL=$(cat /proc/device-tree/model | tr '\0' '\n')
        log_info "Detected: $PI_MODEL"

        case "$PI_MODEL" in
            *"Raspberry Pi 5"*)
                PI_TYPE="pi5"
                ;;
            *"Raspberry Pi 4"*)
                PI_TYPE="pi4"
                ;;
            *"Raspberry Pi 3"*)
                PI_TYPE="pi3"
                ;;
            *"Raspberry Pi Zero 2"*)
                PI_TYPE="pizero2"
                ;;
            *"Raspberry Pi Zero"*)
                PI_TYPE="pizero"
                ;;
            *)
                PI_TYPE="generic"
                ;;
        esac
    else
        log_warn "Could not detect Raspberry Pi model"
        PI_TYPE="generic"
    fi
}

# Detect architecture
detect_arch() {
    ARCH=$(uname -m)
    case "$ARCH" in
        aarch64|arm64)
            DEB_ARCH="arm64"
            ;;
        armv7l|armhf)
            DEB_ARCH="armhf"
            ;;
        x86_64)
            DEB_ARCH="amd64"
            ;;
        *)
            log_error "Unsupported architecture: $ARCH"
            exit 1
            ;;
    esac
    log_info "Architecture: $ARCH ($DEB_ARCH)"
}

# Check and install dependencies
install_dependencies() {
    log_step "Installing dependencies..."

    apt-get update -qq

    # Core dependencies
    DEPS="libasound2 libssl3"

    # Check if already installed
    for dep in $DEPS; do
        if ! dpkg -s "$dep" >/dev/null 2>&1; then
            apt-get install -y "$dep"
        fi
    done

    log_info "Dependencies installed"
}

# Create system user
create_user() {
    log_step "Creating system user..."

    if ! id -u "$SOLUNA_USER" >/dev/null 2>&1; then
        useradd --system --no-create-home --shell /usr/sbin/nologin \
            --groups audio "$SOLUNA_USER"
        log_info "Created user: $SOLUNA_USER"
    else
        # Ensure user is in audio group
        usermod -a -G audio "$SOLUNA_USER"
        log_info "User $SOLUNA_USER already exists"
    fi
}

# Create directories
create_directories() {
    log_step "Creating directories..."

    mkdir -p "$CONFIG_DIR"
    mkdir -p "$LOG_DIR"
    mkdir -p "$DATA_DIR"

    chown "$SOLUNA_USER:$SOLUNA_GROUP" "$LOG_DIR"
    chown "$SOLUNA_USER:$SOLUNA_GROUP" "$DATA_DIR"

    log_info "Directories created"
}

# Download and install package
install_package() {
    log_step "Installing Soluna v${SOLUNA_VERSION}..."

    DEB_URL="https://github.com/yukihamada/opensonic/releases/download/v${SOLUNA_VERSION}/soluna_${SOLUNA_VERSION}_${DEB_ARCH}.deb"
    DEB_FILE="/tmp/soluna_${SOLUNA_VERSION}_${DEB_ARCH}.deb"

    # Validate downloaded file before installing
    validate_download() {
        local file="$1"
        if [[ ! -f "$file" ]] || [[ $(stat -c%s "$file" 2>/dev/null || stat -f%z "$file" 2>/dev/null) -lt 1024 ]]; then
            return 1
        fi
        return 0
    }

    log_info "Downloading from: $DEB_URL"

    if command -v curl &>/dev/null; then
        curl -sSL -o "$DEB_FILE" "$DEB_URL" || {
            log_warn "Download failed, trying to build from source..."
            build_from_source
            return
        }
    elif command -v wget &>/dev/null; then
        wget -q -O "$DEB_FILE" "$DEB_URL" || {
            log_warn "Download failed, trying to build from source..."
            build_from_source
            return
        }
    else
        log_warn "Neither curl nor wget found, building from source..."
        build_from_source
        return
    fi

    dpkg -i "$DEB_FILE"
    rm -f "$DEB_FILE"

    log_info "Package installed"
}

# Build from source (fallback)
build_from_source() {
    log_step "Building from source..."

    # Install build dependencies
    apt-get install -y git cmake g++ libasound2-dev libssl-dev

    TEMP_DIR=$(mktemp -d)
    cd "$TEMP_DIR"

    git clone --depth 1 https://github.com/yukihamada/opensonic.git
    cd soluna

    mkdir build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release -DSOLUNA_BUILD_TESTS=OFF
    make -j$(nproc)
    make install

    cd /
    rm -rf "$TEMP_DIR"

    log_info "Built and installed from source"
}

# Detect audio devices
detect_audio_devices() {
    log_step "Detecting audio devices..."

    if command -v aplay &>/dev/null; then
        echo ""
        echo "Available audio output devices:"
        aplay -l 2>/dev/null | grep "^card" || echo "  (none found)"
        echo ""

        echo "Available audio input devices:"
        arecord -l 2>/dev/null | grep "^card" || echo "  (none found)"
        echo ""
    fi

    # Try to find default device
    if [[ -f /proc/asound/cards ]]; then
        DEFAULT_CARD=$(head -1 /proc/asound/cards | awk '{print $1}')
        if [[ -n "$DEFAULT_CARD" ]]; then
            AUDIO_DEVICE="hw:$DEFAULT_CARD"
            log_info "Default audio device: $AUDIO_DEVICE"
        fi
    fi

    # Check for HiFiBerry
    if grep -q "hifiberry" /proc/asound/cards 2>/dev/null; then
        log_info "HiFiBerry DAC detected"
        AUDIO_DEVICE="hw:sndrpihifiberry"
    fi
}

# Generate configuration
generate_config() {
    log_step "Generating configuration..."

    if [[ -f "$CONFIG_DIR/config.yaml" ]]; then
        log_warn "Configuration already exists, skipping"
        return
    fi

    # Get hostname
    HOSTNAME=$(hostname)

    cat > "$CONFIG_DIR/config.yaml" << EOF
# Soluna Configuration
# Generated by installer on $(date)

device:
  name: "$HOSTNAME"
  audio: "${AUDIO_DEVICE:-default}"

network:
  control_port: 8400
  rtp_base: 5004
  multicast_audio: "239.69.0.1"

audio:
  sample_rate: 48000
  channels: 2
  bit_depth: 24

security:
  dtls: false
  auth_enabled: false

metrics:
  enabled: true
  port: 9100

logging:
  level: info
  file: "/var/log/soluna/daemon.log"

audit:
  enabled: true
  file: "/var/log/soluna/audit.jsonl"
EOF

    chown root:$SOLUNA_GROUP "$CONFIG_DIR/config.yaml"
    chmod 640 "$CONFIG_DIR/config.yaml"

    log_info "Configuration written to $CONFIG_DIR/config.yaml"
}

# Install systemd service
install_service() {
    log_step "Installing systemd service..."

    cat > /etc/systemd/system/soluna.service << 'EOF'
[Unit]
Description=Soluna Network Audio Daemon
Documentation=https://github.com/yukihamada/opensonic
After=network-online.target sound.target
Wants=network-online.target

[Service]
Type=simple
User=soluna
Group=audio
ExecStart=/usr/bin/solunad --config /etc/soluna/config.yaml
ExecReload=/bin/kill -HUP $MAINPID
Restart=always
RestartSec=5
TimeoutStopSec=10

# Performance tuning
Nice=-10
LimitRTPRIO=99
LimitMEMLOCK=infinity

# Security hardening
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
ReadWritePaths=/var/log/soluna /var/lib/soluna

# Audio access
SupplementaryGroups=audio

[Install]
WantedBy=multi-user.target
EOF

    systemctl daemon-reload
    systemctl enable soluna.service

    log_info "Systemd service installed and enabled"
}

# Start service
start_service() {
    log_step "Starting Soluna service..."

    systemctl start soluna.service

    sleep 2

    if systemctl is-active --quiet soluna.service; then
        log_info "Soluna is running"
    else
        log_warn "Service may have failed to start. Check: journalctl -u soluna"
    fi
}

# Print summary
print_summary() {
    echo ""
    echo "=============================================="
    echo -e "${GREEN}Soluna installation complete!${NC}"
    echo "=============================================="
    echo ""
    echo "Configuration: $CONFIG_DIR/config.yaml"
    echo "Logs:          $LOG_DIR/"
    echo ""
    echo "Commands:"
    echo "  sudo systemctl status soluna    # Check status"
    echo "  sudo systemctl restart soluna   # Restart"
    echo "  sudo journalctl -u soluna -f    # View logs"
    echo ""
    echo "Web UI: http://$(hostname -I | awk '{print $1}'):8400/"
    echo ""
    echo "Edit $CONFIG_DIR/config.yaml to customize settings."
    echo ""
}

# Main installation
main() {
    echo ""
    echo "=============================================="
    echo "  Soluna Raspberry Pi Installer v${SOLUNA_VERSION}"
    echo "=============================================="
    echo ""

    check_root
    detect_pi_model
    detect_arch
    install_dependencies
    create_user
    create_directories
    install_package
    detect_audio_devices
    generate_config
    install_service
    start_service
    print_summary
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --version)
            echo "Soluna Installer v${SOLUNA_VERSION}"
            exit 0
            ;;
        --help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --version     Show version"
            echo "  --help        Show this help"
            echo ""
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            exit 1
            ;;
    esac
    shift
done

main
