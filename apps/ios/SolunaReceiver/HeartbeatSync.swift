//
//  HeartbeatSync.swift
//  SolunaReceiver
//
//  Heartbeat Sync: Reads heart rate from HealthKit (if authorized) and
//  broadcasts via TEXT:heartbeat protocol. Shows all listeners' heartbeats
//  as pulsing circles.
//

import SwiftUI
import HealthKit

@MainActor
class HeartbeatManager: ObservableObject {
    static let shared = HeartbeatManager()

    @Published var myBPM: Int = 0
    @Published var otherBPMs: [(id: String, bpm: Int)] = []
    @Published var averageBPM: Int = 0
    @Published var isSynced: Bool = false  // true when all hearts are within +/-5 BPM

    private let healthStore = HKHealthStore()
    private var query: HKAnchoredObjectQuery?

    func start(sendUDP: @escaping (String) -> Void) {
        guard HKHealthStore.isHealthDataAvailable() else { return }
        let heartRateType = HKQuantityType.quantityType(forIdentifier: .heartRate)!

        healthStore.requestAuthorization(toShare: nil, read: [heartRateType]) { [weak self] granted, _ in
            guard granted else { return }
            DispatchQueue.main.async { self?.startHeartRateQuery(sendUDP: sendUDP) }
        }
    }

    private func startHeartRateQuery(sendUDP: @escaping (String) -> Void) {
        let heartRateType = HKQuantityType.quantityType(forIdentifier: .heartRate)!
        let query = HKAnchoredObjectQuery(type: heartRateType, predicate: nil, anchor: nil, limit: HKObjectQueryNoLimit) { [weak self] _, samples, _, _, _ in
            guard let self else { return }
            let capturedSamples = samples
            let capturedSendUDP = sendUDP
            Task { @MainActor in
                self.processSamples(capturedSamples, sendUDP: capturedSendUDP)
            }
        }
        query.updateHandler = { [weak self] _, samples, _, _, _ in
            guard let self else { return }
            let capturedSamples = samples
            let capturedSendUDP = sendUDP
            Task { @MainActor in
                self.processSamples(capturedSamples, sendUDP: capturedSendUDP)
            }
        }
        healthStore.execute(query)
        self.query = query
    }

    private func processSamples(_ samples: [HKSample]?, sendUDP: @escaping (String) -> Void) {
        guard let samples = samples as? [HKQuantitySample], let latest = samples.last else { return }
        let bpm = Int(latest.quantity.doubleValue(for: HKUnit(from: "count/min")))
        DispatchQueue.main.async { [weak self] in
            self?.myBPM = bpm
            sendUDP("TEXT:heartbeat \(UIDevice.current.name):\(bpm)\n")
            self?.updateSync()
        }
    }

    func handleRelay(_ text: String) {
        guard text.hasPrefix("TEXT:heartbeat ") else { return }
        let content = String(text.dropFirst("TEXT:heartbeat ".count)).trimmingCharacters(in: .whitespacesAndNewlines)
        if let colonIdx = content.firstIndex(of: ":"), let bpm = Int(content[content.index(after: colonIdx)...]) {
            let name = String(content[..<colonIdx])
            if let idx = otherBPMs.firstIndex(where: { $0.id == name }) {
                otherBPMs[idx].bpm = bpm
            } else {
                otherBPMs.append((name, bpm))
            }
            updateSync()
        }
    }

    private func updateSync() {
        let allBPMs = [myBPM] + otherBPMs.map(\.bpm)
        let valid = allBPMs.filter { $0 > 0 }
        guard !valid.isEmpty else { return }
        averageBPM = valid.reduce(0, +) / valid.count
        let maxDiff = valid.map { abs($0 - averageBPM) }.max() ?? 0
        isSynced = maxDiff <= 5
    }

    func stop() {
        if let q = query { healthStore.stop(q) }
        query = nil
    }
}

struct HeartbeatView: View {
    @ObservedObject var hb = HeartbeatManager.shared
    @State private var pulseScale: CGFloat = 1.0

    var body: some View {
        HStack(spacing: 8) {
            // Pulsing heart
            Image(systemName: "heart.fill")
                .font(.system(size: 12))
                .foregroundColor(.red)
                .scaleEffect(pulseScale)
                .onAppear {
                    withAnimation(.easeInOut(duration: hb.myBPM > 0 ? 60.0 / Double(hb.myBPM) : 1.0).repeatForever(autoreverses: true)) {
                        pulseScale = 1.3
                    }
                }

            if hb.myBPM > 0 {
                Text("\(hb.myBPM)")
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundColor(.red.opacity(0.7))
            }

            if hb.isSynced && hb.otherBPMs.count > 0 {
                Text("IN SYNC")
                    .font(.system(size: 8, weight: .black, design: .monospaced))
                    .foregroundColor(.green)
            }
        }
    }
}
