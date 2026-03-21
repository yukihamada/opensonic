//
//  SleepTimer.swift
//  Soluna
//
//  Sleep timer — gradually fades out audio and stops playback
//

import Foundation

@MainActor
class SleepTimerManager: ObservableObject {
    static let shared = SleepTimerManager()

    @Published var isActive = false
    @Published var remainingSeconds: Int = 0
    @Published var selectedMinutes: Int = 0

    private var timer: Timer?

    func start(minutes: Int) {
        stop()
        selectedMinutes = minutes
        remainingSeconds = minutes * 60
        isActive = true
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self, self.isActive else { return }
                self.remainingSeconds -= 1
                if self.remainingSeconds <= 0 {
                    self.expire()
                }
                // Fade out volume in last 30 seconds
                if self.remainingSeconds <= 30 && self.remainingSeconds > 0 {
                    let fade = Float(self.remainingSeconds) / 30.0
                    NotificationCenter.default.post(name: .solunaSleepFade, object: nil, userInfo: ["fade": fade])
                }
            }
        }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
        isActive = false
        remainingSeconds = 0
    }

    private func expire() {
        stop()
        // Post notification to stop playback
        NotificationCenter.default.post(name: .solunaIntentStop, object: nil)
    }

    var formattedRemaining: String {
        let m = remainingSeconds / 60
        let s = remainingSeconds % 60
        return String(format: "%d:%02d", m, s)
    }
}

extension Notification.Name {
    static let solunaSleepFade = Notification.Name("solunaSleepFade")
}
