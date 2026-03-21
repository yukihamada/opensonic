//
//  ChatManager.swift
//  SolunaReceiver
//
//  Real-time text chat via the relay server's TEXT:chat protocol.
//

import Foundation
import UIKit

@MainActor
class ChatManager: ObservableObject {
    static let shared = ChatManager()

    struct Message: Identifiable {
        let id = UUID()
        let sender: String
        let text: String
        let timestamp: Date
        let isMe: Bool
    }

    @Published var messages: [Message] = []
    @Published var unreadCount: Int = 0

    /// Parse incoming TEXT:chat messages from the relay.
    /// Format: "TEXT:chat <sender>: <message>\n"
    func handleRelayMessage(_ text: String) {
        guard text.hasPrefix("TEXT:chat ") else { return }
        let content = String(text.dropFirst("TEXT:chat ".count)).trimmingCharacters(in: .newlines)
        if let colonIdx = content.firstIndex(of: ":") {
            let sender = String(content[..<colonIdx]).trimmingCharacters(in: .whitespaces)
            let msg = String(content[content.index(after: colonIdx)...]).trimmingCharacters(in: .whitespaces)
            // Skip echo of own messages (relay broadcasts to all including sender)
            let deviceName = UIDevice.current.name
            if sender == deviceName { return }
            messages.append(Message(sender: sender, text: msg, timestamp: Date(), isMe: false))
            unreadCount += 1
        }
    }

    /// Send a chat message via the relay.
    func send(_ text: String, via sendUDP: (String) -> Void) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        let deviceName = UIDevice.current.name
        sendUDP("TEXT:chat \(deviceName): \(trimmed)\n")
        messages.append(Message(sender: "Me", text: trimmed, timestamp: Date(), isMe: true))
    }

    /// Reset unread count (called when chat view appears).
    func markAsRead() {
        unreadCount = 0
    }
}
