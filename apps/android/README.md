# Soluna Receiver - Android

Minimal Android app for receiving audio from a Soluna relay server via the OSTP (Open Soluna Transport Protocol) over UDP.

## Features

- Connect to any Soluna relay server via UDP
- Receive and play OSTP/RTP audio (48kHz stereo int32 PCM)
- Display connection status, channel name, member count
- Query wallet balance
- Send tips to DJs
- Material3 / Jetpack Compose UI
- Automatic keepalive (re-JOIN every 10s to avoid 15s stale timeout)

## Requirements

- Android 8.0+ (API 26)
- Android Studio Ladybug or newer
- JDK 17

## Build

```bash
cd apps/android
./gradlew assembleDebug
```

Or open the `apps/android/` directory in Android Studio.

## Protocol

The app implements the OSTP client protocol:

1. Sends `JOIN:<channel>::<device_name>\n` to the relay server via UDP
2. Receives `OK:joined\n` confirmation
3. Receives RTP/OSTP audio packets (24-byte header + int32 PCM payload)
4. Sends `WALLET\n` to query balance, `TIP:<amount>\n` to tip, `MEMBERS\n` to list members

### Packet structure

```
RTP Header (12 bytes):
  - Version (2), Padding, Extension, CSRC count
  - Marker, Payload Type (96 = PCM24)
  - Sequence number (16-bit)
  - Timestamp (32-bit)
  - SSRC (32-bit)

RTP Extension Header (4 bytes):
  - Profile: 0x4F53 ("OS")
  - Length: 2 (32-bit words)

OSTP Header (8 bytes):
  - Stream ID (16-bit)
  - Sequence extension (16-bit)
  - Media timestamp (32-bit)

Payload:
  - Interleaved int32 PCM samples, big-endian
  - Stereo: L, R, L, R, ...

Optional CRC-32 trailer (4 bytes)
```

## Architecture

| File | Purpose |
|------|---------|
| `MainActivity.kt` | Activity entry point, creates client and sets Compose content |
| `SolunaClient.kt` | UDP socket management, OSTP packet parsing, relay protocol |
| `AudioPlayer.kt` | AudioTrack wrapper, int32-to-float32 conversion, 48kHz stereo |
| `ui/MainScreen.kt` | Compose UI with server input, status display, tip dialog |
