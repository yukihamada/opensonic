#!/bin/bash
# Soluna RX launcher — reads /etc/soluna-rx.conf for runtime config
# Used by soluna-rx.service so channel/codec/volume can be changed via web UI

set -e

CONFIG=/etc/soluna-rx.conf

# Defaults
RELAY="52.194.128.180:5100"
CHANNEL="jazz"
CODEC="pcm"
ALSA_DEVICE="plughw:1,0"

if [ -f "$CONFIG" ]; then
    while IFS='=' read -r key val; do
        case "$key" in
            RELAY)        RELAY="$val" ;;
            CHANNEL)      CHANNEL="$val" ;;
            CODEC)        CODEC="$val" ;;
            ALSA_DEVICE)  ALSA_DEVICE="$val" ;;
        esac
    done < "$CONFIG"
fi

exec /usr/local/bin/soluna \
    --relay "$RELAY" \
    --group-name "$CHANNEL" \
    --codec "$CODEC" \
    --channels 2 \
    --output alsa \
    --device "$ALSA_DEVICE" \
    --buffer 200
