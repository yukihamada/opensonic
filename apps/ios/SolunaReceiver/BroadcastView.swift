//
//  BroadcastView.swift
//  Soluna — Music file broadcast to any channel via relay
//

import SwiftUI
import UniformTypeIdentifiers

struct BroadcastView: View {
    @StateObject private var manager = MusicBroadcastManager.shared
    @State private var channel: String = UserDefaults.standard.string(forKey: "channel") ?? "my-radio"
    @State private var showFilePicker = false
    @State private var selectedFileURL: URL?
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            ZStack {
                LinearGradient.solunaBg.ignoresSafeArea()
                ScrollView {
                    VStack(spacing: 24) {
                        // MARK: - Track Info
                        VStack(spacing: 12) {
                            ZStack {
                                Circle()
                                    .fill(
                                        RadialGradient(
                                            colors: [.solunaSol.opacity(0.4), .solunaLuna.opacity(0.2), .clear],
                                            center: .center, startRadius: 10, endRadius: 60
                                        )
                                    )
                                    .frame(width: 120, height: 120)

                                Image(systemName: manager.isBroadcasting ? "antenna.radiowaves.left.and.right" : "music.note")
                                    .font(.system(size: 40, weight: .medium))
                                    .foregroundColor(manager.isBroadcasting ? .solunaSol : .white.opacity(0.5))
                                    .opacity(manager.isBroadcasting ? 1.0 : 0.5)
                            }

                            if !manager.currentTrack.isEmpty {
                                Text(manager.currentTrack)
                                    .font(.system(size: 16, weight: .bold))
                                    .foregroundColor(.white)
                                    .lineLimit(2)
                                    .multilineTextAlignment(.center)
                            } else {
                                Text("No file selected")
                                    .font(.system(size: 15))
                                    .foregroundColor(.white.opacity(0.4))
                            }
                        }
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 20)
                        .glassCard()

                        // MARK: - Progress
                        if manager.isBroadcasting {
                            VStack(spacing: 8) {
                                ProgressView(value: manager.progress)
                                    .tint(LinearGradient.solLunaGradient)

                                HStack {
                                    Text(String(format: "%.0f%%", manager.progress * 100))
                                        .font(.system(size: 12, weight: .medium, design: .monospaced))
                                        .foregroundColor(.white.opacity(0.5))
                                    Spacer()
                                    HStack(spacing: 4) {
                                        Circle()
                                            .fill(Color.solunaLive)
                                            .frame(width: 6, height: 6)
                                            .shadow(color: .solunaLive.opacity(0.6), radius: 4)
                                        Text("LIVE on \(manager.broadcastChannel)")
                                            .font(.system(size: 12, weight: .bold))
                                            .foregroundColor(.solunaLive)
                                    }
                                }
                            }
                            .padding(16)
                            .glassCard()
                        }

                        // MARK: - Channel Input
                        VStack(spacing: 8) {
                            Text("Channel")
                                .font(.system(size: 13, weight: .medium))
                                .foregroundColor(.white.opacity(0.4))

                            TextField("Channel name", text: $channel)
                                .font(.system(size: 16, weight: .medium, design: .monospaced))
                                .multilineTextAlignment(.center)
                                .padding(.horizontal, 16)
                                .padding(.vertical, 10)
                                .background(Color.white.opacity(0.06))
                                .clipShape(RoundedRectangle(cornerRadius: 10))
                                .disabled(manager.isBroadcasting)
                                .opacity(manager.isBroadcasting ? 0.5 : 1.0)
                        }
                        .padding(16)
                        .glassCard()

                        // MARK: - File Picker Button
                        Button {
                            showFilePicker = true
                        } label: {
                            HStack(spacing: 10) {
                                Image(systemName: "folder.fill")
                                    .font(.system(size: 16))
                                Text(selectedFileURL != nil ? "Change File" : "Pick Audio File")
                                    .font(.system(size: 15, weight: .semibold))
                            }
                            .foregroundColor(.solunaLuna)
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 14)
                            .background(Color.solunaLuna.opacity(0.12))
                            .clipShape(RoundedRectangle(cornerRadius: 14))
                            .overlay(
                                RoundedRectangle(cornerRadius: 14)
                                    .strokeBorder(Color.solunaLuna.opacity(0.3), lineWidth: 0.5)
                            )
                        }
                        .disabled(manager.isBroadcasting)
                        .opacity(manager.isBroadcasting ? 0.5 : 1.0)

                        // MARK: - Start / Stop Button
                        Button {
                            if manager.isBroadcasting {
                                manager.stop()
                            } else if let url = selectedFileURL {
                                let ch = channel.trimmingCharacters(in: .whitespaces)
                                guard !ch.isEmpty else { return }
                                manager.start(fileURL: url, channel: ch)
                            }
                        } label: {
                            HStack(spacing: 10) {
                                Image(systemName: manager.isBroadcasting ? "stop.fill" : "antenna.radiowaves.left.and.right")
                                    .font(.system(size: 18, weight: .bold))
                                Text(manager.isBroadcasting ? "Stop Broadcast" : "Start Broadcast")
                                    .font(.system(size: 17, weight: .bold))
                            }
                            .foregroundColor(.white)
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 16)
                            .background(
                                manager.isBroadcasting
                                    ? AnyShapeStyle(Color.solunaMic.gradient)
                                    : AnyShapeStyle(LinearGradient.solGradient)
                            )
                            .clipShape(RoundedRectangle(cornerRadius: 16))
                            .shadow(
                                color: manager.isBroadcasting ? .solunaMic.opacity(0.3) : .solunaSol.opacity(0.3),
                                radius: 12, y: 4
                            )
                        }
                        .disabled(!manager.isBroadcasting && selectedFileURL == nil)
                        .opacity(!manager.isBroadcasting && selectedFileURL == nil ? 0.5 : 1.0)

                        Spacer().frame(height: 20)
                    }
                    .padding(.horizontal, 16)
                    .padding(.top, 12)
                }
            }
            .navigationTitle("Music Broadcast")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Done") { dismiss() }
                        .foregroundColor(.solunaLuna)
                }
            }
            .fileImporter(
                isPresented: $showFilePicker,
                allowedContentTypes: [
                    UTType.mp3,
                    UTType.mpeg4Audio,
                    UTType.wav,
                    UTType.audio
                ],
                allowsMultipleSelection: false
            ) { result in
                if case .success(let urls) = result, let url = urls.first {
                    if url.startAccessingSecurityScopedResource() {
                        selectedFileURL = url
                        manager.currentTrack = url.lastPathComponent
                    }
                }
            }
        }
        .preferredColorScheme(.dark)
    }
}

#Preview {
    BroadcastView()
}
