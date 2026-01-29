# Soluna Receiver for iOS

iOS app for receiving Soluna/AES67 network audio streams.

## Features

- Multicast RTP audio reception (239.69.0.1:5004)
- Auto-detection of OSTP and AES67 packet formats
- Low-latency CoreAudio playback via RemoteIO
- Background audio support
- Volume control

## Requirements

- iOS 14.0+
- Xcode 14.0+
- Same WiFi network as the transmitter
- Router must allow multicast traffic

## Build Instructions

### Option 1: Using Xcode Project

1. Open `SolunaReceiver.xcodeproj` in Xcode
2. Select your development team in Signing & Capabilities
3. Build and run on your device

### Option 2: Using CMake (Experimental)

```bash
# From the opensonic root directory
mkdir build-ios && cd build-ios
cmake .. -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DSOLUNA_BUILD_IOS=ON
open Soluna.xcodeproj
```

## Project Structure

```
apps/ios/
├── SolunaReceiver.xcodeproj/     # Xcode project
├── SolunaReceiver/
│   ├── Info.plist                # App configuration
│   ├── SolunaReceiverApp.swift   # App entry point
│   ├── ContentView.swift         # Main UI
│   ├── AudioReceiver.swift       # Swift audio manager
│   └── Bridge/
│       ├── SolunaReceiver-Bridging-Header.h
│       ├── AudioReceiverBridge.h   # Obj-C interface
│       └── AudioReceiverBridge.mm  # C++/Obj-C++ implementation
└── README.md
```

## Usage

1. **Start the transmitter on Mac:**
   ```bash
   ./build/solunad --tx --device "MacBook Pro Microphone"
   ```

2. **Launch Soluna Rx on iPhone**

3. **Tap the play button** to start receiving audio

4. **Adjust volume** using the slider

## Network Configuration

The app uses multicast address `239.69.0.1` on port `5004` by default.

For multicast to work:
- Both devices must be on the same WiFi network
- Your router must not block multicast traffic
- If using a mesh network, ensure multicast forwarding is enabled

## Troubleshooting

### No audio received
- Verify both devices are on the same network
- Check that the transmitter is running
- Try disabling any VPN connections
- Check router multicast settings

### Audio dropouts
- Move closer to the WiFi access point
- Reduce network congestion
- Try a 5GHz WiFi band if available

### App crashes on start
- Ensure the app has Local Network permission (Settings > Privacy > Local Network)
- Grant microphone permission if prompted (even though we only play audio, iOS may request it)

## Architecture

```
┌─────────────────────────────────────────────┐
│              SwiftUI (ContentView)          │
│                     │                       │
│              AudioReceiver.swift            │
│                     │                       │
├─────────────────────┼───────────────────────┤
│              Bridging Header                │
│                     │                       │
│         AudioReceiverBridge.mm              │
│         (Objective-C++)                     │
│                     │                       │
│    ┌────────────────┼────────────────┐      │
│    │                │                │      │
│ RtpReceiver    RingBuffer     CoreAudio     │
│ (UDP/RTP)    (Lock-free)    (RemoteIO)      │
└────────────────────────────────────────────┘
```

## License

MIT License - See repository root for details.
