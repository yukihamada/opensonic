//
//  SoundTeleport.swift
//  SolunaReceiver
//
//  Sound Teleport: Record 1 second from mic, encode as S24-in-S32LE RTP,
//  and broadcast via relay so all listeners hear it simultaneously.
//

import AVFoundation
import SwiftUI

@MainActor
class SoundTeleportManager: ObservableObject {
    static let shared = SoundTeleportManager()

    @Published var isRecording = false
    @Published var countdown: Int = 0  // 3, 2, 1, GO!

    private var audioEngine: AVAudioEngine?
    private var recordedSamples: [Float] = []

    /// Record 1 second from mic, then broadcast via relay
    func teleport(sendPacket: @escaping ([UInt8]) -> Void) {
        guard !isRecording else { return }

        // 3-2-1 countdown
        countdown = 3
        isRecording = true

        func tick(_ n: Int) {
            if n > 0 {
                DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
                    self?.countdown = n - 1
                    tick(n - 1)
                }
            } else {
                self.startCapture(sendPacket: sendPacket)
            }
        }
        tick(3)
    }

    private func startCapture(sendPacket: @escaping ([UInt8]) -> Void) {
        let engine = AVAudioEngine()
        let input = engine.inputNode
        let format = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 48000, channels: 1, interleaved: false)!

        recordedSamples = []

        input.installTap(onBus: 0, bufferSize: 4800, format: format) { [weak self] buffer, _ in
            guard let self else { return }
            if let data = buffer.floatChannelData?[0] {
                let count = Int(buffer.frameLength)
                for i in 0..<count {
                    self.recordedSamples.append(data[i])
                }
            }
            // Stop after 1 second (48000 samples)
            if self.recordedSamples.count >= 48000 {
                DispatchQueue.main.async {
                    self.stopAndSend(sendPacket: sendPacket)
                }
            }
        }

        do {
            try engine.start()
            self.audioEngine = engine
        } catch {
            isRecording = false
        }
    }

    private func stopAndSend(sendPacket: @escaping ([UInt8]) -> Void) {
        audioEngine?.inputNode.removeTap(onBus: 0)
        audioEngine?.stop()
        audioEngine = nil

        // Send recorded samples as RTP packets (96 samples per packet)
        let samplesPerPacket = 96
        var seq: UInt16 = 0
        var ts: UInt32 = 0

        var i = 0
        // Use a timer to send at correct rate (~2ms per packet)
        Timer.scheduledTimer(withTimeInterval: Double(samplesPerPacket) / 48000.0, repeats: true) { [weak self] timer in
            guard let self, i < self.recordedSamples.count else {
                timer.invalidate()
                DispatchQueue.main.async { self?.isRecording = false }
                return
            }

            var packet = [UInt8]()
            // RTP header
            packet.append(0x80)
            packet.append(96) // PT=96
            packet.append(UInt8(seq >> 8))
            packet.append(UInt8(seq & 0xFF))
            let tsBE = ts.bigEndian
            withUnsafeBytes(of: tsBE) { packet.append(contentsOf: $0) }
            packet.append(contentsOf: [0x00, 0x00, 0x00, 0x02]) // SSRC=2 (teleport)

            // S24-in-S32LE payload
            let end = min(i + samplesPerPacket, self.recordedSamples.count)
            for j in i..<end {
                let scaled = Int32(max(-1.0, min(1.0, self.recordedSamples[j])) * 8388607.0)
                packet.append(UInt8(truncatingIfNeeded: scaled))
                packet.append(UInt8(truncatingIfNeeded: scaled >> 8))
                packet.append(UInt8(truncatingIfNeeded: scaled >> 16))
                packet.append(UInt8(truncatingIfNeeded: scaled >> 24))
            }

            // CRC placeholder
            packet.append(contentsOf: [0, 0, 0, 0])

            sendPacket(packet)
            i += samplesPerPacket
            seq += 1
            ts += UInt32(samplesPerPacket)
        }
    }
}

struct TeleportButton: View {
    @ObservedObject var manager = SoundTeleportManager.shared
    let sendPacket: ([UInt8]) -> Void

    var body: some View {
        Button {
            if !manager.isRecording {
                AVCaptureDevice.requestAccess(for: .audio) { granted in
                    if granted {
                        DispatchQueue.main.async {
                            manager.teleport(sendPacket: sendPacket)
                        }
                    }
                }
            }
        } label: {
            ZStack {
                if manager.isRecording {
                    if manager.countdown > 0 {
                        Text("\(manager.countdown)")
                            .font(.system(size: 18, weight: .black, design: .rounded))
                            .foregroundColor(.red)
                    } else {
                        Circle().fill(.red.opacity(0.3)).frame(width: 32, height: 32)
                            .overlay(Circle().stroke(.red, lineWidth: 2))
                    }
                } else {
                    Image(systemName: "megaphone.fill")
                        .font(.system(size: 13))
                        .foregroundColor(.orange)
                }
            }
            .frame(width: 32, height: 32)
            .background(Color.orange.opacity(0.12))
            .clipShape(Circle())
        }
        .disabled(manager.isRecording)
    }
}
