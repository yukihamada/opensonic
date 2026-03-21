//
//  SilentDiscoView.swift
//  Soluna — Silent Disco QR Code Generator & Scanner
//

import SwiftUI
import CoreImage.CIFilterBuiltins
import AVFoundation

// MARK: - Silent Disco View

@available(iOS 16.0, *)
struct SilentDiscoView: View {
    let channel: String
    let onJoinChannel: (String) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var mode: DiscoMode = .share

    enum DiscoMode: String, CaseIterable {
        case share = "Share"
        case scan = "Scan"
    }

    private var channelURL: String {
        "https://relay.solun.art/c/\(channel)"
    }

    private var channelEmoji: String {
        let emojis: [String: String] = [
            "soluna": "\u{2600}\u{FE0F}", "jazz": "\u{1F3B9}", "lofi": "\u{1F3A7}",
            "chill": "\u{1F343}", "dance": "\u{26A1}", "bjj": "\u{1F94B}", "yuki": "\u{2744}\u{FE0F}"
        ]
        return emojis[channel] ?? "\u{1F4FB}"
    }

    var body: some View {
        NavigationView {
            ZStack {
                Color.black.ignoresSafeArea()

                VStack(spacing: 0) {
                    // Mode picker
                    Picker("Mode", selection: $mode) {
                        ForEach(DiscoMode.allCases, id: \.self) { m in
                            Text(m.rawValue).tag(m)
                        }
                    }
                    .pickerStyle(.segmented)
                    .padding(.horizontal, 40)
                    .padding(.top, 16)
                    .padding(.bottom, 24)

                    switch mode {
                    case .share:
                        shareView
                    case .scan:
                        SilentDiscoScannerView(onJoinChannel: { ch in
                            onJoinChannel(ch)
                            dismiss()
                        })
                    }

                    Spacer()
                }
            }
            .preferredColorScheme(.dark)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                        .foregroundColor(.white.opacity(0.7))
                }
            }
        }
    }

    // MARK: - Share View (QR Generator)

    private var shareView: some View {
        VStack(spacing: 24) {
            // Channel emoji + name
            Text(channelEmoji)
                .font(.system(size: 56))
                .shadow(color: .purple.opacity(0.6), radius: 20)

            Text(channel.capitalized)
                .font(.system(size: 28, weight: .bold, design: .rounded))
                .foregroundStyle(
                    LinearGradient(
                        colors: [.purple, .cyan],
                        startPoint: .leading,
                        endPoint: .trailing
                    )
                )

            // QR Code
            if let qrImage = generateQR(for: channelURL) {
                ZStack {
                    RoundedRectangle(cornerRadius: 20)
                        .fill(
                            LinearGradient(
                                colors: [
                                    Color.purple.opacity(0.15),
                                    Color.cyan.opacity(0.1),
                                    Color.purple.opacity(0.05)
                                ],
                                startPoint: .topLeading,
                                endPoint: .bottomTrailing
                            )
                        )
                        .overlay(
                            RoundedRectangle(cornerRadius: 20)
                                .strokeBorder(
                                    LinearGradient(
                                        colors: [.purple.opacity(0.5), .cyan.opacity(0.3)],
                                        startPoint: .topLeading,
                                        endPoint: .bottomTrailing
                                    ),
                                    lineWidth: 1
                                )
                        )
                        .shadow(color: .purple.opacity(0.3), radius: 20, y: 8)

                    Image(uiImage: qrImage)
                        .interpolation(.none)
                        .resizable()
                        .scaledToFit()
                        .padding(24)
                        .colorInvert() // white QR on dark background
                }
                .frame(width: 260, height: 260)
            }

            // Scan to Join label
            HStack(spacing: 8) {
                Image(systemName: "qrcode.viewfinder")
                    .font(.system(size: 16, weight: .medium))
                Text("Scan to Join")
                    .font(.system(size: 17, weight: .semibold))
            }
            .foregroundStyle(
                LinearGradient(
                    colors: [.purple, .cyan],
                    startPoint: .leading,
                    endPoint: .trailing
                )
            )

            // Copy link button
            Button {
                UIPasteboard.general.string = channelURL
            } label: {
                Label("Copy Link", systemImage: "doc.on.doc")
                    .font(.subheadline.weight(.medium))
                    .foregroundColor(.white.opacity(0.7))
                    .padding(.horizontal, 20)
                    .padding(.vertical, 10)
                    .background(Color.white.opacity(0.08))
                    .clipShape(Capsule())
                    .overlay(
                        Capsule().strokeBorder(Color.white.opacity(0.1), lineWidth: 0.5)
                    )
            }
        }
    }

    // MARK: - QR Generation

    private func generateQR(for string: String) -> UIImage? {
        let context = CIContext()
        let filter = CIFilter.qrCodeGenerator()
        filter.message = Data(string.utf8)
        filter.correctionLevel = "M"

        guard let outputImage = filter.outputImage else { return nil }
        let scaled = outputImage.transformed(by: CGAffineTransform(scaleX: 10, y: 10))
        guard let cgImage = context.createCGImage(scaled, from: scaled.extent) else { return nil }
        return UIImage(cgImage: cgImage)
    }
}

// MARK: - QR Scanner View

@available(iOS 16.0, *)
struct SilentDiscoScannerView: UIViewControllerRepresentable {
    let onJoinChannel: (String) -> Void

    func makeUIViewController(context: Context) -> ScannerViewController {
        let vc = ScannerViewController()
        vc.onChannelDetected = onJoinChannel
        return vc
    }

    func updateUIViewController(_ uiViewController: ScannerViewController, context: Context) {}

    class ScannerViewController: UIViewController, AVCaptureMetadataOutputObjectsDelegate {
        var onChannelDetected: ((String) -> Void)?
        private var captureSession: AVCaptureSession?
        private var previewLayer: AVCaptureVideoPreviewLayer?
        private var hasDetected = false

        override func viewDidLoad() {
            super.viewDidLoad()
            view.backgroundColor = .black
            setupCamera()
        }

        override func viewDidLayoutSubviews() {
            super.viewDidLayoutSubviews()
            previewLayer?.frame = view.bounds
        }

        override func viewWillDisappear(_ animated: Bool) {
            super.viewWillDisappear(animated)
            captureSession?.stopRunning()
        }

        private func setupCamera() {
            let session = AVCaptureSession()

            guard let device = AVCaptureDevice.default(for: .video),
                  let input = try? AVCaptureDeviceInput(device: device) else {
                showPlaceholder("Camera not available")
                return
            }

            if session.canAddInput(input) {
                session.addInput(input)
            }

            let output = AVCaptureMetadataOutput()
            if session.canAddOutput(output) {
                session.addOutput(output)
                output.setMetadataObjectsDelegate(self, queue: .main)
                output.metadataObjectTypes = [.qr]
            }

            let layer = AVCaptureVideoPreviewLayer(session: session)
            layer.videoGravity = .resizeAspectFill
            layer.frame = view.bounds
            view.layer.addSublayer(layer)
            previewLayer = layer
            captureSession = session

            // Overlay: scanning frame
            let overlay = ScannerOverlayView(frame: view.bounds)
            overlay.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            view.addSubview(overlay)

            DispatchQueue.global(qos: .userInitiated).async {
                session.startRunning()
            }
        }

        private func showPlaceholder(_ text: String) {
            let label = UILabel()
            label.text = text
            label.textColor = .white
            label.textAlignment = .center
            label.font = .systemFont(ofSize: 17, weight: .medium)
            label.translatesAutoresizingMaskIntoConstraints = false
            view.addSubview(label)
            NSLayoutConstraint.activate([
                label.centerXAnchor.constraint(equalTo: view.centerXAnchor),
                label.centerYAnchor.constraint(equalTo: view.centerYAnchor)
            ])
        }

        // MARK: - AVCaptureMetadataOutputObjectsDelegate

        func metadataOutput(_ output: AVCaptureMetadataOutput,
                            didOutput metadataObjects: [AVMetadataObject],
                            from connection: AVCaptureConnection) {
            guard !hasDetected,
                  let object = metadataObjects.first as? AVMetadataMachineReadableCodeObject,
                  let value = object.stringValue else { return }

            if let channel = extractChannel(from: value) {
                hasDetected = true
                captureSession?.stopRunning()

                // Haptic feedback
                let generator = UINotificationFeedbackGenerator()
                generator.notificationOccurred(.success)

                onChannelDetected?(channel)
            }
        }

        /// Extract channel name from URL patterns:
        /// - https://relay.solun.art/c/{channel}
        /// - soluna://join/{channel}
        /// - soluna://channel/{channel}
        private func extractChannel(from string: String) -> String? {
            // relay URL
            if let range = string.range(of: "relay.solun.art/c/") {
                let ch = String(string[range.upperBound...])
                    .trimmingCharacters(in: CharacterSet.alphanumerics.inverted)
                return ch.isEmpty ? nil : ch
            }
            // soluna:// deep link
            if string.hasPrefix("soluna://") {
                let path = string.replacingOccurrences(of: "soluna://", with: "")
                let parts = path.split(separator: "/")
                if parts.count >= 2, (parts[0] == "join" || parts[0] == "channel") {
                    let ch = String(parts[1])
                    return ch.isEmpty ? nil : ch
                }
            }
            return nil
        }
    }
}

// MARK: - Scanner Overlay

private class ScannerOverlayView: UIView {
    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .clear
        isUserInteractionEnabled = false
    }

    required init?(coder: NSCoder) { fatalError() }

    override func draw(_ rect: CGRect) {
        guard let ctx = UIGraphicsGetCurrentContext() else { return }

        // Semi-transparent overlay
        ctx.setFillColor(UIColor.black.withAlphaComponent(0.5).cgColor)
        ctx.fill(rect)

        // Clear center square
        let size: CGFloat = min(rect.width, rect.height) * 0.65
        let center = CGRect(
            x: (rect.width - size) / 2,
            y: (rect.height - size) / 2,
            width: size,
            height: size
        )
        ctx.setBlendMode(.clear)
        let path = UIBezierPath(roundedRect: center, cornerRadius: 20)
        ctx.addPath(path.cgPath)
        ctx.fillPath()

        // Corner accents
        ctx.setBlendMode(.normal)
        let cornerLength: CGFloat = 30
        let lineWidth: CGFloat = 3
        let purple = UIColor.purple.cgColor
        let cyan = UIColor.cyan.cgColor

        ctx.setLineWidth(lineWidth)
        ctx.setLineCap(.round)

        // Top-left
        ctx.setStrokeColor(purple)
        ctx.move(to: CGPoint(x: center.minX, y: center.minY + cornerLength))
        ctx.addLine(to: CGPoint(x: center.minX, y: center.minY + 10))
        ctx.addQuadCurve(to: CGPoint(x: center.minX + 10, y: center.minY),
                         control: CGPoint(x: center.minX, y: center.minY))
        ctx.addLine(to: CGPoint(x: center.minX + cornerLength, y: center.minY))
        ctx.strokePath()

        // Top-right
        ctx.setStrokeColor(cyan)
        ctx.move(to: CGPoint(x: center.maxX - cornerLength, y: center.minY))
        ctx.addLine(to: CGPoint(x: center.maxX - 10, y: center.minY))
        ctx.addQuadCurve(to: CGPoint(x: center.maxX, y: center.minY + 10),
                         control: CGPoint(x: center.maxX, y: center.minY))
        ctx.addLine(to: CGPoint(x: center.maxX, y: center.minY + cornerLength))
        ctx.strokePath()

        // Bottom-left
        ctx.setStrokeColor(cyan)
        ctx.move(to: CGPoint(x: center.minX, y: center.maxY - cornerLength))
        ctx.addLine(to: CGPoint(x: center.minX, y: center.maxY - 10))
        ctx.addQuadCurve(to: CGPoint(x: center.minX + 10, y: center.maxY),
                         control: CGPoint(x: center.minX, y: center.maxY))
        ctx.addLine(to: CGPoint(x: center.minX + cornerLength, y: center.maxY))
        ctx.strokePath()

        // Bottom-right
        ctx.setStrokeColor(purple)
        ctx.move(to: CGPoint(x: center.maxX - cornerLength, y: center.maxY))
        ctx.addLine(to: CGPoint(x: center.maxX - 10, y: center.maxY))
        ctx.addQuadCurve(to: CGPoint(x: center.maxX, y: center.maxY - 10),
                         control: CGPoint(x: center.maxX, y: center.maxY))
        ctx.addLine(to: CGPoint(x: center.maxX, y: center.maxY - cornerLength))
        ctx.strokePath()

        // "Point camera at QR code" label
        let labelRect = CGRect(x: 0, y: center.maxY + 24, width: rect.width, height: 30)
        let paragraphStyle = NSMutableParagraphStyle()
        paragraphStyle.alignment = .center
        let attrs: [NSAttributedString.Key: Any] = [
            .font: UIFont.systemFont(ofSize: 15, weight: .medium),
            .foregroundColor: UIColor.white.withAlphaComponent(0.6),
            .paragraphStyle: paragraphStyle
        ]
        "Point camera at QR code".draw(in: labelRect, withAttributes: attrs)
    }
}
