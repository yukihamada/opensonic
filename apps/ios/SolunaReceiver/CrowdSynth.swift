//
//  CrowdSynth.swift
//  SolunaReceiver
//
//  Crowd Synth: Each iPhone becomes one note of a synthesizer.
//  Tilt the phone to change your note. The combined sound is a chord.
//

import CoreMotion
import AVFoundation
import SwiftUI

@MainActor
class CrowdSynthManager: ObservableObject {
    static let shared = CrowdSynthManager()

    @Published var isActive = false
    @Published var currentNote: String = "C4"
    @Published var frequency: Float = 261.63

    private let motionManager = CMMotionManager()
    private var synthEngine: AVAudioEngine?
    private var synthNode: AVAudioSourceNode?
    private var phase: Float = 0

    // Map pitch angle to musical notes
    let notes: [(name: String, freq: Float)] = [
        ("C4", 261.63), ("D4", 293.66), ("E4", 329.63), ("F4", 349.23),
        ("G4", 392.00), ("A4", 440.00), ("B4", 493.88), ("C5", 523.25),
    ]

    func start() {
        guard !isActive else { return }
        isActive = true
        startSynth()
        startMotion()
    }

    func stop() {
        isActive = false
        motionManager.stopDeviceMotionUpdates()
        synthEngine?.stop()
        if let node = synthNode, let eng = synthEngine { eng.detach(node) }
        synthEngine = nil
        synthNode = nil
    }

    private func startMotion() {
        guard motionManager.isDeviceMotionAvailable else { return }
        motionManager.deviceMotionUpdateInterval = 0.1
        motionManager.startDeviceMotionUpdates(to: .main) { [weak self] motion, _ in
            guard let self, let motion else { return }
            // Map pitch (-90 to +90 degrees) to note index
            let pitch = motion.attitude.pitch // radians, -pi/2 to +pi/2
            let normalized = Float((pitch + .pi/2) / .pi) // 0.0 to 1.0
            let idx = min(self.notes.count - 1, max(0, Int(normalized * Float(self.notes.count))))
            self.frequency = self.notes[idx].freq
            self.currentNote = self.notes[idx].name
        }
    }

    private func startSynth() {
        let engine = AVAudioEngine()
        let format = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 48000, channels: 1, interleaved: false)!

        let node = AVAudioSourceNode(format: format) { [weak self] _, _, frameCount, bufferList -> OSStatus in
            guard let self else { return noErr }
            let ablp = UnsafeMutableAudioBufferListPointer(bufferList)
            let frames = Int(frameCount)
            guard let dst = ablp[0].mData?.assumingMemoryBound(to: Float.self) else { return noErr }

            let freq = self.frequency
            let phaseIncrement = freq / 48000.0
            var p = self.phase

            for i in 0..<frames {
                // Soft sine wave
                dst[i] = sin(p * 2.0 * .pi) * 0.3
                p += phaseIncrement
                if p >= 1.0 { p -= 1.0 }
            }
            self.phase = p
            return noErr
        }

        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: format)
        engine.mainMixerNode.outputVolume = 0.5

        do { try engine.start() } catch { return }
        synthEngine = engine
        synthNode = node
    }
}

struct CrowdSynthView: View {
    @ObservedObject var synth = CrowdSynthManager.shared
    @Environment(\.dismiss) var dismiss

    var body: some View {
        NavigationStack {
            ZStack {
                LinearGradient.solunaBg.ignoresSafeArea()
                VStack(spacing: 24) {
                    Text("Tilt your phone to play")
                        .font(.system(size: 14)).foregroundColor(.white.opacity(0.5))

                    // Big note display
                    Text(synth.currentNote)
                        .font(.system(size: 72, weight: .black, design: .rounded))
                        .foregroundStyle(LinearGradient.solLunaGradient)

                    Text(String(format: "%.0f Hz", synth.frequency))
                        .font(.system(size: 16, design: .monospaced))
                        .foregroundColor(.white.opacity(0.4))

                    // Piano key visualization
                    HStack(spacing: 3) {
                        ForEach(Array(synth.notes.enumerated()), id: \.offset) { _, note in
                            let active = synth.currentNote == note.name
                            RoundedRectangle(cornerRadius: 4)
                                .fill(active ? LinearGradient.solGradient : LinearGradient(colors: [Color.white.opacity(0.1)], startPoint: .top, endPoint: .bottom))
                                .frame(height: active ? 100 : 80)
                                .overlay(
                                    Text(note.name)
                                        .font(.system(size: 10, weight: .bold))
                                        .foregroundColor(active ? .white : .white.opacity(0.3))
                                        .padding(.bottom, 8),
                                    alignment: .bottom
                                )
                        }
                    }
                    .padding(.horizontal, 20)

                    // Start/Stop
                    Button {
                        if synth.isActive { synth.stop() } else { synth.start() }
                        UIImpactFeedbackGenerator(style: .heavy).impactOccurred()
                    } label: {
                        Text(synth.isActive ? "Stop" : "Play")
                            .font(.system(size: 17, weight: .bold))
                            .foregroundColor(.white)
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 14)
                            .background(synth.isActive ? Color.red.opacity(0.8) : Color.solunaSol)
                            .clipShape(RoundedRectangle(cornerRadius: 14))
                    }
                    .padding(.horizontal, 40)

                    Text("Everyone in the channel hears a chord\nmade from all players' notes")
                        .font(.system(size: 12)).foregroundColor(.white.opacity(0.3))
                        .multilineTextAlignment(.center)
                }
                .padding(24)
            }
            .navigationTitle("Crowd Synth")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { synth.stop(); dismiss() }
                }
            }
        }
    }
}
