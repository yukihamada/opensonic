//
//  ChannelQRView.swift
//  Soluna
//

import SwiftUI
import CoreImage.CIFilterBuiltins

@available(iOS 15.0, *)
struct ChannelQRView: View {
    let channel: String
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationView {
            VStack(spacing: 32) {
                Text("Share Channel")
                    .font(.title2.weight(.bold))

                if let qrImage = generateQR(for: "soluna://join/\(channel)") {
                    Image(uiImage: qrImage)
                        .interpolation(.none)
                        .resizable()
                        .scaledToFit()
                        .frame(width: 240, height: 240)
                        .cornerRadius(12)
                }

                HStack(spacing: 8) {
                    Image(systemName: "dot.radiowaves.left.and.right")
                        .foregroundColor(.purple)
                    Text(channel)
                        .font(.title3.weight(.semibold))
                }

                Text("Scan this QR code with another device\nto join the same channel.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .multilineTextAlignment(.center)

                // Copy link button
                Button {
                    UIPasteboard.general.string = "soluna://join/\(channel)"
                } label: {
                    Label("Copy Link", systemImage: "doc.on.doc")
                        .font(.subheadline.weight(.medium))
                        .padding(.horizontal, 20)
                        .padding(.vertical, 10)
                        .background(Color(.tertiarySystemFill))
                        .clipShape(Capsule())
                }
            }
            .padding()
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                }
            }
        }
    }

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
