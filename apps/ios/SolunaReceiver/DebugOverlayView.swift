import SwiftUI

/// Debug overlay showing real-time audio stats
struct DebugOverlayView: View {
    @ObservedObject var receiver: AudioReceiver
    @State private var timer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 4) {
                Text("DEBUG").font(.system(size: 11, weight: .bold, design: .monospaced)).foregroundColor(.red)

                Group {
                    row("State", "\(receiver.state)")
                    row("Packets RX", "\(receiver.packetsReceived)")
                    row("Packets Drop", "\(receiver.packetsDropped)")
                    row("Output Level", String(format: "%.3f", receiver.outputLevel))
                    row("Stream Mode", UserDefaults.standard.string(forKey: "streamMode") ?? "sync")
                    row("Channel", UserDefaults.standard.string(forKey: "channel") ?? "soluna")
                    row("Now Playing", receiver.nowPlayingTitle ?? "-")
                    row("Relay Error", receiver.relayError ?? "none")
                    row("Version", "\(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?") (\(Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "?"))")
                }

                if !receiver.debugLog.isEmpty {
                    Divider().background(Color.gray)
                    Text("RELAY LOG").font(.system(size: 10, weight: .bold, design: .monospaced)).foregroundColor(.orange)
                    let lines = receiver.debugLog.components(separatedBy: "\n").filter { !$0.isEmpty }.suffix(15)
                    ForEach(Array(lines.enumerated()), id: \.offset) { _, line in
                        Text(line)
                            .font(.system(size: 9, design: .monospaced))
                            .foregroundColor(.green.opacity(0.8))
                            .lineLimit(2)
                    }
                }
            }
            .padding(12)
        }
        .background(Color.black.opacity(0.9))
        .cornerRadius(12)
    }

    private func row(_ label: String, _ value: String) -> some View {
        HStack(spacing: 4) {
            Text(label).font(.system(size: 10, design: .monospaced)).foregroundColor(.gray)
            Spacer()
            Text(value).font(.system(size: 10, weight: .medium, design: .monospaced)).foregroundColor(.white)
        }
    }
}
