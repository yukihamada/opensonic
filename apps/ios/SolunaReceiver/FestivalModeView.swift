import SwiftUI
import CoreImage.CIFilterBuiltins

struct FestivalModeView: View {
    @ObservedObject var receiver: AudioReceiver
    @Environment(\.dismiss) private var dismiss
    @State private var pulse: CGFloat = 1.0
    @State private var hue: Double = 0.6

    private var channel: String {
        UserDefaults.standard.string(forKey: "channel") ?? "soluna"
    }

    var body: some View {
        ZStack {
            // Pulsing gradient background synced to audio
            LinearGradient(
                colors: [
                    Color(hue: hue, saturation: 0.85, brightness: max(0.15, Double(receiver.outputLevel) * 0.9)),
                    Color(hue: hue + 0.15, saturation: 0.9, brightness: max(0.05, Double(receiver.outputLevel) * 0.6))
                ],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
            .scaleEffect(pulse)
            .ignoresSafeArea()

            // Content
            VStack(spacing: 24) {
                Spacer()

                // Beat circle
                Circle()
                    .fill(.white.opacity(Double(receiver.outputLevel) * 0.9 + 0.1))
                    .frame(width: 140, height: 140)
                    .scaleEffect(pulse)
                    .shadow(color: .white.opacity(0.4), radius: 30)

                // Channel name
                Text(channel)
                    .font(.system(size: 52, weight: .black, design: .rounded))
                    .foregroundColor(.white)
                    .shadow(color: .black.opacity(0.6), radius: 10)

                // QR code for others to join
                if let qr = generateQR(from: "soluna://channel/\(channel)") {
                    Image(uiImage: qr)
                        .interpolation(.none)
                        .resizable()
                        .frame(width: 120, height: 120)
                        .cornerRadius(8)
                        .shadow(color: .white.opacity(0.3), radius: 10)

                    Text("Scan to join")
                        .font(.caption)
                        .foregroundColor(.white.opacity(0.7))
                }

                Spacer()
            }

            // Close button
            VStack {
                HStack {
                    Spacer()
                    Button { dismiss() } label: {
                        Image(systemName: "xmark.circle.fill")
                            .font(.system(size: 30))
                            .foregroundColor(.white.opacity(0.5))
                    }
                    .padding(20)
                }
                Spacer()
            }
        }
        .onChange(of: receiver.outputLevel) { level in
            withAnimation(.easeOut(duration: 0.08)) {
                pulse = 1.0 + CGFloat(level) * 0.4
                hue = 0.55 + Double(level) * 0.35
            }
        }
        .onAppear {
            UIScreen.main.brightness = 1.0
            UIApplication.shared.isIdleTimerDisabled = true
        }
        .onDisappear {
            UIScreen.main.brightness = 0.5
            UIApplication.shared.isIdleTimerDisabled = false
        }
        .statusBarHidden()
    }

    private func generateQR(from string: String) -> UIImage? {
        guard let data = string.data(using: .ascii) else { return nil }
        let filter = CIFilter.qrCodeGenerator()
        filter.setValue(data, forKey: "inputMessage")
        filter.setValue("M", forKey: "inputCorrectionLevel")
        guard let output = filter.outputImage else { return nil }
        let scaled = output.transformed(by: CGAffineTransform(scaleX: 10, y: 10))
        return UIImage(ciImage: scaled)
    }
}
