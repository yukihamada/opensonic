#!/bin/bash
#
# Soluna Uninstaller
#
# SPDX-License-Identifier: MIT
#

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check root
if [[ $EUID -ne 0 ]]; then
    log_error "This script must be run as root (use sudo)"
    exit 1
fi

echo ""
echo "This will remove Soluna from your system."
echo ""
read -p "Do you want to keep configuration files? [Y/n] " -n 1 -r
echo ""

KEEP_CONFIG=true
if [[ $REPLY =~ ^[Nn]$ ]]; then
    KEEP_CONFIG=false
fi

# Stop and disable service
log_info "Stopping Soluna service..."
systemctl stop soluna.service 2>/dev/null || true
systemctl disable soluna.service 2>/dev/null || true
rm -f /etc/systemd/system/soluna.service
systemctl daemon-reload

# Remove package
log_info "Removing package..."
if dpkg -s soluna >/dev/null 2>&1; then
    dpkg -r soluna
fi

# Remove binaries (if installed from source)
rm -f /usr/bin/solunad
rm -f /usr/bin/solctl

# Remove config if requested
if [[ "$KEEP_CONFIG" = false ]]; then
    log_info "Removing configuration..."
    rm -rf /etc/soluna
fi

# Remove logs and data
log_info "Removing logs and data..."
rm -rf /var/log/soluna
rm -rf /var/lib/soluna

# Remove user (optional)
read -p "Remove soluna user? [y/N] " -n 1 -r
echo ""
if [[ $REPLY =~ ^[Yy]$ ]]; then
    userdel soluna 2>/dev/null || true
    log_info "User removed"
fi

echo ""
log_info "Soluna has been uninstalled."
if [[ "$KEEP_CONFIG" = true ]]; then
    echo "Configuration preserved at /etc/soluna/"
fi
echo ""
