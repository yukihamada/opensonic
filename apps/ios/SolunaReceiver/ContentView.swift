//
//  ContentView.swift
//  SolunaReceiver
//
//  Main UI for Soluna network audio receiver
//

import SwiftUI

struct ContentView: View {
    @StateObject private var receiver = AudioReceiver()

    var body: some View {
        VStack(spacing: 24) {
            // Header
            Text("Soluna Rx")
                .font(.largeTitle)
                .fontWeight(.bold)
                .padding(.top, 40)

            Spacer()

            // Play/Stop button
            Button(action: {
                receiver.toggle()
            }) {
                ZStack {
                    Circle()
                        .fill(buttonColor)
                        .frame(width: 100, height: 100)
                        .shadow(radius: 4)

                    Image(systemName: receiver.isPlaying ? "stop.fill" : "play.fill")
                        .font(.system(size: 40))
                        .foregroundColor(.white)
                        .offset(x: receiver.isPlaying ? 0 : 4)
                }
            }
            .disabled(receiver.state == .connecting)

            // Status section
            VStack(spacing: 8) {
                HStack {
                    Circle()
                        .fill(statusColor)
                        .frame(width: 10, height: 10)
                    Text(receiver.state.rawValue)
                        .font(.headline)
                }

                if let error = receiver.errorMessage {
                    Text(error)
                        .font(.caption)
                        .foregroundColor(.red)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal)
                }

                // Packet stats
                HStack(spacing: 20) {
                    VStack {
                        Text(formatNumber(receiver.packetsReceived))
                            .font(.system(.title2, design: .monospaced))
                            .fontWeight(.medium)
                        Text("Packets")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }

                    if receiver.packetsDropped > 0 {
                        VStack {
                            Text(formatNumber(receiver.packetsDropped))
                                .font(.system(.title2, design: .monospaced))
                                .fontWeight(.medium)
                                .foregroundColor(.orange)
                            Text("Dropped")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                    }
                }
                .padding(.top, 8)
            }
            .padding()
            .background(Color(.systemGray6))
            .cornerRadius(12)

            Spacer()

            // Volume control
            VStack(spacing: 8) {
                HStack {
                    Image(systemName: "speaker.fill")
                        .foregroundColor(.secondary)
                    Slider(value: $receiver.volume, in: 0...1)
                    Image(systemName: "speaker.wave.3.fill")
                        .foregroundColor(.secondary)
                }
                Text("Volume: \(Int(receiver.volume * 100))%")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            .padding(.horizontal, 24)
            .padding(.bottom, 40)
        }
        .padding()
    }

    private var buttonColor: Color {
        switch receiver.state {
        case .receiving:
            return .red
        case .connecting:
            return .orange
        case .error:
            return .gray
        case .stopped:
            return .blue
        }
    }

    private var statusColor: Color {
        switch receiver.state {
        case .receiving:
            return .green
        case .connecting:
            return .orange
        case .error:
            return .red
        case .stopped:
            return .gray
        }
    }

    private func formatNumber(_ value: UInt64) -> String {
        if value >= 1_000_000 {
            return String(format: "%.1fM", Double(value) / 1_000_000)
        } else if value >= 1_000 {
            return String(format: "%.1fK", Double(value) / 1_000)
        } else {
            return "\(value)"
        }
    }
}

#Preview {
    ContentView()
}
