//
//  TipManager.swift
//  SolunaReceiver
//
//  Manages tipping/support for DJs via relay TIP command.
//  Uses notification pattern to send UDP messages through the relay.
//

import Foundation
import Network

extension Notification.Name {
    static let solunaSendTip = Notification.Name("solunaSendTip")
}

@MainActor
class TipManager: ObservableObject {
    static let shared = TipManager()

    @Published var totalTipped: Int = 0
    @Published var tipAnimations: [TipAnimation] = []

    struct TipAnimation: Identifiable {
        let id = UUID()
        let amount: Int
        let emoji: String
    }

    private static let relayHost = "relay.solun.art"
    private static let relayPort: UInt16 = 5100

    /// Send a tip to the DJ via relay UDP
    func sendTip(amount: Int, djDeviceId: String) {
        let message = "TIP:\(amount):\(djDeviceId)\n"
        sendRelayMessage(message)

        totalTipped += amount

        let emoji = amount >= 1000 ? "\u{1F48E}" : amount >= 500 ? "\u{1F525}" : "\u{2764}\u{FE0F}"
        let animation = TipAnimation(amount: amount, emoji: emoji)
        tipAnimations.append(animation)

        let animId = animation.id
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
            self?.tipAnimations.removeAll { $0.id == animId }
        }
    }

    /// Send a support contribution to the DJ's royalty costs
    func sendSupport(amount: Int, djDeviceId: String) {
        let message = "SUPPORT:\(amount):\(djDeviceId)\n"
        sendRelayMessage(message)

        totalTipped += amount

        let animation = TipAnimation(amount: amount, emoji: "\u{1F31F}")
        tipAnimations.append(animation)

        let animId = animation.id
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
            self?.tipAnimations.removeAll { $0.id == animId }
        }
    }

    /// Reset session totals
    func resetSession() {
        totalTipped = 0
        tipAnimations.removeAll()
    }

    // MARK: - UDP Send (fire-and-forget)

    private func sendRelayMessage(_ message: String) {
        let host = NWEndpoint.Host(Self.relayHost)
        guard let port = NWEndpoint.Port(rawValue: Self.relayPort) else { return }
        let connection = NWConnection(host: host, port: port, using: .udp)

        connection.stateUpdateHandler = { state in
            switch state {
            case .ready:
                guard let data = message.data(using: .utf8) else {
                    connection.cancel()
                    return
                }
                connection.send(content: data, completion: .contentProcessed { _ in
                    connection.cancel()
                })
            case .failed, .cancelled:
                break
            default:
                break
            }
        }
        connection.start(queue: .global(qos: .userInitiated))
    }
}
