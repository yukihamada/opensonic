//
//  AIAutoChannel.swift
//  Soluna
//
//  Uses CoreMotion (accelerometer) + time of day to suggest/auto-switch channels
//

import SwiftUI
import CoreMotion

@MainActor
class AIAutoChannel: ObservableObject {
    static let shared = AIAutoChannel()

    @Published var suggestedChannel: String = "soluna"
    @Published var isAutoMode: Bool = false {
        didSet {
            UserDefaults.standard.set(isAutoMode, forKey: "aiAutoChannel")
            if isAutoMode { start() } else { stop() }
        }
    }
    @Published var activityLevel: String = "relaxing"  // relaxing, walking, running
    @Published var suggestionReason: String = ""

    private let motionManager = CMMotionActivityManager()
    private var timer: Timer?

    init() {
        isAutoMode = UserDefaults.standard.bool(forKey: "aiAutoChannel")
        if isAutoMode { updateSuggestion() }
    }

    func start() {
        guard isAutoMode else { return }

        // Monitor activity
        if CMMotionActivityManager.isActivityAvailable() {
            motionManager.startActivityUpdates(to: .main) { [weak self] activity in
                guard let activity else { return }
                Task { @MainActor in
                    guard let self else { return }
                    if activity.running {
                        self.activityLevel = "running"
                    } else if activity.walking {
                        self.activityLevel = "walking"
                    } else {
                        self.activityLevel = "relaxing"
                    }
                    self.updateSuggestion()
                }
            }
        }

        // Update every 5 minutes
        timer = Timer.scheduledTimer(withTimeInterval: 300, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.updateSuggestion() }
        }
        updateSuggestion()
    }

    func stop() {
        motionManager.stopActivityUpdates()
        timer?.invalidate()
        timer = nil
    }

    private func updateSuggestion() {
        let hour = Calendar.current.component(.hour, from: Date())

        switch activityLevel {
        case "running":
            suggestedChannel = "dance"
            suggestionReason = "High energy for workouts"
        case "walking":
            if hour < 12 {
                suggestedChannel = "lofi"
                suggestionReason = "Morning walk vibes"
            } else {
                suggestedChannel = "jazz"
                suggestionReason = "Afternoon stroll"
            }
        default:
            // Time-based for relaxing
            switch hour {
            case 6..<9:
                suggestedChannel = "chill"
                suggestionReason = "Morning calm"
            case 9..<12:
                suggestedChannel = "lofi"
                suggestionReason = "Focus time"
            case 12..<14:
                suggestedChannel = "jazz"
                suggestionReason = "Lunch vibes"
            case 14..<18:
                suggestedChannel = "soluna"
                suggestionReason = "Afternoon mix"
            case 18..<21:
                suggestedChannel = "dance"
                suggestionReason = "Evening energy"
            case 21..<24:
                suggestedChannel = "chill"
                suggestionReason = "Wind down"
            default:
                suggestedChannel = "lofi"
                suggestionReason = "Late night"
            }
        }
    }
}

// MARK: - AI Suggestion Chip

struct AISuggestionChip: View {
    let channel: String
    let reason: String
    let onTap: () -> Void

    private var channelEmoji: String {
        let emojis: [String: String] = [
            "soluna": "☀️", "jazz": "🎹", "lofi": "🎧",
            "chill": "🍃", "dance": "⚡", "bjj": "🥋"
        ]
        return emojis[channel] ?? "📻"
    }

    var body: some View {
        Button(action: onTap) {
            HStack(spacing: 6) {
                Text("AI")
                    .font(.system(size: 9, weight: .heavy, design: .rounded))
                    .foregroundColor(.white)
                    .padding(.horizontal, 5)
                    .padding(.vertical, 2)
                    .background(LinearGradient.solLunaGradient)
                    .clipShape(RoundedRectangle(cornerRadius: 4))
                Text("Try \(channel.capitalized) \(channelEmoji)")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(.white.opacity(0.8))
                Text("(\(reason))")
                    .font(.system(size: 10))
                    .foregroundColor(.white.opacity(0.4))
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(Color.white.opacity(0.06))
            .clipShape(Capsule())
            .overlay(
                Capsule()
                    .strokeBorder(Color.white.opacity(0.08), lineWidth: 0.5)
            )
        }
        .buttonStyle(.plain)
    }
}

// MARK: - AI Toggle Button (for header)

struct AIToggleButton: View {
    @Binding var isEnabled: Bool

    var body: some View {
        Button {
            isEnabled.toggle()
        } label: {
            Text("AI")
                .font(.system(size: 10, weight: .heavy, design: .rounded))
                .foregroundColor(isEnabled ? .white : .white.opacity(0.5))
                .frame(width: 28, height: 28)
                .background(isEnabled ? AnyShapeStyle(LinearGradient.solLunaGradient) : AnyShapeStyle(Color.white.opacity(0.08)))
                .clipShape(Circle())
        }
    }
}

#if DEBUG
struct AIAutoChannel_Previews: PreviewProvider {
    static var previews: some View {
        VStack(spacing: 16) {
            AIToggleButton(isEnabled: .constant(false))
            AIToggleButton(isEnabled: .constant(true))
            AISuggestionChip(channel: "jazz", reason: "Lunch vibes") {}
            AISuggestionChip(channel: "dance", reason: "High energy for workouts") {}
        }
        .padding()
        .background(Color.black)
        .preferredColorScheme(.dark)
    }
}
#endif
