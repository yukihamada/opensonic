//
//  ChatView.swift
//  SolunaReceiver
//
//  Real-time text chat UI — glass-card dark theme.
//

import SwiftUI

struct ChatView: View {
    @ObservedObject var chatManager = ChatManager.shared
    let sendUDP: (String) -> Void
    @State private var draft = ""
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Text("Chat").font(.system(size: 20, weight: .bold)).foregroundColor(.white)
                Spacer()
                Button { dismiss() } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 22))
                        .foregroundColor(.white.opacity(0.4))
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)

            Divider().background(Color.white.opacity(0.08))

            // Messages
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(spacing: 8) {
                        ForEach(chatManager.messages) { msg in
                            MessageBubble(message: msg)
                                .id(msg.id)
                        }
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                }
                .onChange(of: chatManager.messages.count) { _ in
                    if let last = chatManager.messages.last {
                        withAnimation(.easeOut(duration: 0.2)) {
                            proxy.scrollTo(last.id, anchor: .bottom)
                        }
                    }
                }
            }

            Divider().background(Color.white.opacity(0.08))

            // Input bar
            HStack(spacing: 10) {
                TextField("Message...", text: $draft)
                    .font(.system(size: 15))
                    .foregroundColor(.white)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 10)
                    .background(Color.white.opacity(0.06))
                    .clipShape(RoundedRectangle(cornerRadius: 20))
                    .submitLabel(.send)
                    .onSubmit { sendMessage() }

                Button(action: sendMessage) {
                    Image(systemName: "arrow.up.circle.fill")
                        .font(.system(size: 30))
                        .foregroundStyle(
                            LinearGradient(
                                colors: draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
                                    ? [.white.opacity(0.2), .white.opacity(0.2)]
                                    : [.solunaLuna, .solunaLunaEnd],
                                startPoint: .topLeading, endPoint: .bottomTrailing)
                        )
                }
                .disabled(draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
        }
        .background(Color.solunaSurface.ignoresSafeArea())
        .onAppear { chatManager.markAsRead() }
    }

    private func sendMessage() {
        let text = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        chatManager.send(text, via: sendUDP)
        draft = ""
    }
}

// MARK: - Message Bubble

private struct MessageBubble: View {
    let message: ChatManager.Message

    private var timeString: String {
        let fmt = DateFormatter()
        fmt.dateFormat = "HH:mm"
        return fmt.string(from: message.timestamp)
    }

    var body: some View {
        HStack(alignment: .bottom, spacing: 6) {
            if message.isMe { Spacer(minLength: 48) }

            VStack(alignment: message.isMe ? .trailing : .leading, spacing: 3) {
                if !message.isMe {
                    Text(message.sender)
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(.solunaLuna)
                }
                Text(message.text)
                    .font(.system(size: 15))
                    .foregroundColor(.white)
                    .multilineTextAlignment(message.isMe ? .trailing : .leading)
                Text(timeString)
                    .font(.system(size: 10))
                    .foregroundColor(.white.opacity(0.3))
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(
                RoundedRectangle(cornerRadius: 16)
                    .fill(message.isMe
                          ? AnyShapeStyle(LinearGradient(colors: [.solunaLuna.opacity(0.25), .solunaLunaEnd.opacity(0.15)],
                                                          startPoint: .topLeading, endPoint: .bottomTrailing))
                          : AnyShapeStyle(Color.white.opacity(0.06)))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 16)
                    .strokeBorder(Color.white.opacity(0.06), lineWidth: 0.5)
            )

            if !message.isMe { Spacer(minLength: 48) }
        }
    }
}
