# Lessons Learned

- **Default channels = 1 (Mono)**: All platforms MUST default to mono. User has repeatedly emphasized this. Locations: iOS SettingsView `@AppStorage("channels")`, Mac SettingsView, web `baChannels`, `ba-output-ch` select order, `resetToDefaults()`. Bridge header already says "default: 1".
- **solunad lock file**: Only one instance can run at a time. `pkill -9 solunad` to force stop before re-testing.
- **Relay deploy**: `cd apps/relay && fly deploy -a soluna-relay`. Web deploy: `cp web/* deploy/web/ && cd deploy && fly deploy -a soluna-web`.
- **Stripe webhook**: Created `we_1TAY4dDqLakc8NxkykO9NW85`, URL uses RELAY_WEBHOOK_PATH secret suffix. STRIPE_WEBHOOK_SECRET set for signature verification.
- **Security: device_id vs device_name**: Financial operations (wallets, royalty) must use `device_id` (UUID), not `device_name` (user-provided, spoofable). Channel ownership checks should match both for backward compat.
- **C++ bridge JOIN flood**: heartbeat sends HELLO only (no JOIN re-send). `AudioReceiver.start()` + `connectRelay()` causes duplicate connections.
- **WanRelayClient crash**: close UDP socket BEFORE thread::join() to unblock recvfrom.
- **C++ _impl null**: `receiver.start()` creates _impl. Without it, connectToRelay returns false.
- **iOS channel switch**: crossfadeTo() breaks. Use stop()→delay→start() instead.
- **Mac TX callback**: Always wire tx_relay_callback (wan_relay_send_audio checks state internally).
- **SDKAudioTransmitter**: Pure-Swift TX. inputNode→S24-in-S32LE→RTP(PT=96)+CRC-32→UDP. Connect inputNode→mainMixerNode(vol=0).
- **@MainActor audio thread**: Use nonisolated(unsafe) atomic flag for audio tap reads.
- **NSOpenPanel in sheet**: Use SwiftUI .fileImporter() instead.
- **isBroadcasting stale**: Check SDKAudioTransmitter.isTransmitting only (not C++ bridge state).
- **FFT spectrum**: vDSP_fft_zrip + Hann window, scale 4/N², range -80dB..0dB.
- **ScreenCaptureKit**: Register both audio AND video outputs to avoid drop errors.
- **Soluna Fastlane**: Project=Soluna.xcodeproj, not SolunaReceiver.xcodeproj.
