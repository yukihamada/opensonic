#!/bin/bash
set -e

# Start relay server in background
soluna-relay \
    --port 5100 \
    --stats-interval 60 \
    --max-replay 5000 \
    --wallets-db /data/wallets.json \
    --transactions-log /data/transactions.jsonl \
    --royalty-log /data/royalties.jsonl &

RELAY_PID=$!

# Wait for relay to start
sleep 3

# Multi-channel radio: each subdirectory in /data/music becomes a channel
# /data/music/soluna/*.mp3  → channel "soluna"
# /data/music/lofi/*.mp3    → channel "lofi"
# /data/music/jazz/*.mp3    → channel "jazz"

MUSIC_BASE="${RADIO_DIR:-/data/music}"
mkdir -p "$MUSIC_BASE"

# Also support flat mode: /data/music/*.mp3 → channel "soluna"
flat_count=$(find "$MUSIC_BASE" -maxdepth 1 -name "*.mp3" -type f 2>/dev/null | wc -l)
if [ "$flat_count" -gt 0 ]; then
    mkdir -p "$MUSIC_BASE/soluna"
    find "$MUSIC_BASE" -maxdepth 1 -name "*.mp3" -type f -exec mv {} "$MUSIC_BASE/soluna/" \;
fi

CHANNEL_COUNT=0
for dir in "$MUSIC_BASE"/*/; do
    [ -d "$dir" ] || continue
    channel=$(basename "$dir")
    [ "$channel" = "*" ] && continue  # skip literal asterisk dir
    file_count=$(find "$dir" -maxdepth 1 -name "*.mp3" -type f 2>/dev/null | wc -l)
    if [ "$file_count" -gt 0 ]; then
        echo "[radio] Starting channel '$channel' ($file_count tracks) from $dir"
        soluna-radio --dir "$dir" --relay 127.0.0.1:5100 --channel "$channel" &
        CHANNEL_COUNT=$((CHANNEL_COUNT + 1))
        sleep 1  # stagger starts
    fi
done

if [ "$CHANNEL_COUNT" -eq 0 ]; then
    echo "[radio] No music found. Upload MP3s to /data/music/<channel>/"
fi

echo "[radio] $CHANNEL_COUNT channels active"

# Wait for relay (main process)
wait $RELAY_PID
