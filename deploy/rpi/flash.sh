#!/bin/bash
# Soluna RPi SD Card Flash Script
# Usage: sudo bash flash.sh
set -e

DISK="/dev/disk8"
HOSTNAME="soluna-rpi2"
USERNAME="pi"
SSID="Hama-Fi-IoT"
COUNTRY="JP"
IMAGE_URL="https://downloads.raspberrypi.com/raspios_lite_arm64_latest"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

echo ""
echo "============================================"
echo "  Soluna RPi Flash Script"
echo "  Target: $DISK  Hostname: $HOSTNAME"
echo "============================================"
echo ""

# パスワード入力
read -s -p "WiFi password for '$SSID': " WIFI_PASS; echo
read -s -p "Password for user '$USERNAME': " PI_PASS; echo
echo ""

# 確認
echo -e "${YELLOW}WARNING: $DISK will be COMPLETELY ERASED.${NC}"
diskutil info "$DISK" | grep -E "Size|Media"
read -p "Continue? [y/N] " confirm
[[ "$confirm" != "y" && "$confirm" != "Y" ]] && echo "Aborted." && exit 1

# ダウンロード
IMG_XZ="/tmp/raspios_lite_arm64.img.xz"
IMG="/tmp/raspios_lite_arm64.img"
if [ ! -f "$IMG" ]; then
    echo -e "\n${GREEN}Downloading Raspberry Pi OS Lite (64-bit)...${NC}"
    curl -L --progress-bar -o "$IMG_XZ" "$IMAGE_URL"
    echo "Extracting..."
    xz -d "$IMG_XZ"
fi

# アンマウント
echo -e "\n${GREEN}Unmounting $DISK...${NC}"
diskutil unmountDisk "$DISK" 2>/dev/null || true

# 書き込み
echo -e "${GREEN}Writing image to $DISK... (3-5 min)${NC}"
sudo dd if="$IMG" of="${DISK/disk/rdisk}" bs=4m status=progress 2>&1 || \
sudo dd if="$IMG" of="${DISK/disk/rdisk}" bs=4m
sync
echo -e "${GREEN}Write complete.${NC}"

# マウント待機
echo "Remounting..."
sleep 3
diskutil mountDisk "$DISK" 2>/dev/null || true
sleep 2

# bootfsを探す
BOOT_MOUNT=""
for v in /Volumes/bootfs /Volumes/boot /Volumes/BOOT; do
    [ -d "$v" ] && BOOT_MOUNT="$v" && break
done
[ -z "$BOOT_MOUNT" ] && echo -e "${RED}Boot partition not found.${NC}" && exit 1
echo "Boot partition: $BOOT_MOUNT"

# SSH有効化
touch "$BOOT_MOUNT/ssh"
echo "SSH enabled."

# userconf.txt (ユーザー作成)
PI_HASH=$(echo "$PI_PASS" | openssl passwd -6 -stdin)
echo "${USERNAME}:${PI_HASH}" > "$BOOT_MOUNT/userconf.txt"
echo "User '$USERNAME' configured."

# firstrun.sh (WiFi設定 + ホスト名)
WIFI_PSK=$(wpa_passphrase "$SSID" "$WIFI_PASS" 2>/dev/null | grep "^\s*psk=" | grep -v "#" | awk -F= '{print $2}' || echo "\"$WIFI_PASS\"")

cat > "$BOOT_MOUNT/firstrun.sh" << FIRSTRUN
#!/bin/bash
set +e

# Hostname
echo "$HOSTNAME" > /etc/hostname
sed -i "s/127.0.1.1.*/127.0.1.1\t$HOSTNAME/g" /etc/hosts

# WiFi via NetworkManager (Bookworm)
if command -v nmcli &>/dev/null; then
    nmcli radio wifi on
    cat > /etc/NetworkManager/system-connections/soluna-wifi.nmconnection << 'NMEOF'
[connection]
id=soluna-wifi
type=wifi
autoconnect=true

[wifi]
ssid=$SSID

[wifi-security]
key-mgmt=wpa-psk
psk=$WIFI_PASS

[ipv4]
method=auto

[ipv6]
method=auto
NMEOF
    chmod 600 /etc/NetworkManager/system-connections/soluna-wifi.nmconnection
else
    # Bullseye fallback (wpa_supplicant)
    cat > /etc/wpa_supplicant/wpa_supplicant.conf << 'WPAEOF'
country=$COUNTRY
ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev
update_config=1
network={
    ssid="$SSID"
    psk=$WIFI_PSK
}
WPAEOF
    chmod 600 /etc/wpa_supplicant/wpa_supplicant.conf
    rfkill unblock wifi
fi

# Enable SSH
systemctl enable ssh 2>/dev/null || true

# Cleanup
rm -f /boot/firstrun.sh /boot/firmware/firstrun.sh
sed -i 's| systemd.run.*||g' /boot/cmdline.txt /boot/firmware/cmdline.txt 2>/dev/null
exit 0
FIRSTRUN
chmod +x "$BOOT_MOUNT/firstrun.sh"

# cmdline.txtに firstrun.sh を追加
for cmdline in "$BOOT_MOUNT/cmdline.txt"; do
    [ -f "$cmdline" ] || continue
    if ! grep -q "firstrun" "$cmdline"; then
        sed -i '' 's/$/ systemd.run=\/boot\/firstrun.sh systemd.run_success_action=reboot systemd.unit=kernel-command-line.target/' "$cmdline"
    fi
done

echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  Done! SD card is ready.${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""
echo "1. Eject SD card: diskutil eject $DISK"
echo "2. Insert into RPi and power on"
echo "3. Wait ~60 sec for first boot"
echo "4. SSH: ssh ${USERNAME}@${HOSTNAME}.local"
echo ""
diskutil eject "$DISK" && echo "SD card ejected."
