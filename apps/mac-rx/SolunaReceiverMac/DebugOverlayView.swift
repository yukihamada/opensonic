import SwiftUI

/// Debug overlay showing real-time audio stats
struct DebugOverlayView: View {
    @ObservedObject var receiver: AudioReceiver
    @State private var timer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("DEBUG").font(.system(size: 10, weight: .bold, design: .monospaced)).foregroundColor(.red)

            Group {
                row("State", "\(receiver.state)")
                row("Packets RX", "\(receiver.packetsReceived)")
                row("Packets Drop", "\(receiver.packetsDropped)")
                row("Packets PLC", "\(receiver.packetsConcealed)")
                row("Relay State", "\(receiver.relayState)")
                row("Relay Group", receiver.relayGroup ?? "-")
                row("Relay Error", receiver.relayError ?? "none")
                row("Stream Mode", UserDefaults.standard.string(forKey: "streamMode") ?? "?")
                row("Channel", UserDefaults.standard.string(forKey: "channel") ?? "?")
                row("Version", "\(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?") (\(Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "?"))")
            }
        }
        .padding(8)
        .background(Color.black.opacity(0.85))
        .cornerRadius(8)
        .frame(maxWidth: 350)
    }

    private func row(_ label: String, _ value: String) -> some View {
        HStack(spacing: 4) {
            Text(label).font(.system(size: 9, design: .monospaced)).foregroundColor(.gray)
            Spacer()
            Text(value).font(.system(size: 9, weight: .medium, design: .monospaced)).foregroundColor(.white)
        }
    }
}
