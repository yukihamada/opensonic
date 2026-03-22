//
//  SongRequest.swift
//  SolunaReceiver
//
//  Song request & vote system — listeners request songs via relay TEXT: protocol.
//

import SwiftUI

// MARK: - SongRequestManager

@MainActor
class SongRequestManager: ObservableObject {
    static let shared = SongRequestManager()

    struct Request: Identifiable {
        let id = UUID()
        let title: String
        let requester: String
        var votes: Int
        let timestamp: Date
    }

    @Published var requests: [Request] = []

    func submitRequest(_ title: String, via sendUDP: (String) -> Void) {
        let deviceName = UIDevice.current.name
        sendUDP("TEXT:request \(deviceName):\(title)\n")
        requests.append(Request(title: title, requester: "Me", votes: 1, timestamp: Date()))
    }

    func vote(for request: Request, via sendUDP: (String) -> Void) {
        sendUDP("TEXT:vote \(request.title)\n")
        if let idx = requests.firstIndex(where: { $0.id == request.id }) {
            requests[idx].votes += 1
            requests.sort { $0.votes > $1.votes }
        }
    }

    func handleRelay(_ text: String) {
        if text.hasPrefix("TEXT:request ") {
            let content = String(text.dropFirst("TEXT:request ".count)).trimmingCharacters(in: .whitespacesAndNewlines)
            if let colonIdx = content.firstIndex(of: ":") {
                let requester = String(content[..<colonIdx])
                let title = String(content[content.index(after: colonIdx)...])
                requests.append(Request(title: title, requester: requester, votes: 1, timestamp: Date()))
            }
        } else if text.hasPrefix("TEXT:vote ") {
            let title = String(text.dropFirst("TEXT:vote ".count)).trimmingCharacters(in: .whitespacesAndNewlines)
            if let idx = requests.firstIndex(where: { $0.title == title }) {
                requests[idx].votes += 1
                requests.sort { $0.votes > $1.votes }
            }
        }
    }
}

// MARK: - SongRequestView

struct SongRequestView: View {
    @ObservedObject private var manager = SongRequestManager.shared
    @State private var newRequest = ""
    let sendUDP: (String) -> Void
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            ZStack {
                LinearGradient.solunaBg.ignoresSafeArea()

                VStack(spacing: 16) {
                    // Input field
                    HStack(spacing: 10) {
                        TextField("Request a song...", text: $newRequest)
                            .font(.system(size: 14))
                            .foregroundColor(.white)
                            .padding(.horizontal, 12)
                            .padding(.vertical, 10)
                            .background(Color.white.opacity(0.08))
                            .clipShape(RoundedRectangle(cornerRadius: 10))

                        Button {
                            let title = newRequest.trimmingCharacters(in: .whitespacesAndNewlines)
                            guard !title.isEmpty else { return }
                            manager.submitRequest(title, via: sendUDP)
                            newRequest = ""
                            UIImpactFeedbackGenerator(style: .light).impactOccurred()
                        } label: {
                            Image(systemName: "arrow.up.circle.fill")
                                .font(.system(size: 28))
                                .foregroundStyle(LinearGradient.solGradient)
                        }
                        .disabled(newRequest.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    }
                    .padding(.horizontal, 16)

                    // Request list
                    if manager.requests.isEmpty {
                        Spacer()
                        VStack(spacing: 8) {
                            Image(systemName: "music.note.list")
                                .font(.system(size: 36))
                                .foregroundColor(.white.opacity(0.2))
                            Text("No requests yet")
                                .font(.system(size: 14))
                                .foregroundColor(.white.opacity(0.3))
                        }
                        Spacer()
                    } else {
                        ScrollView {
                            LazyVStack(spacing: 8) {
                                ForEach(manager.requests) { request in
                                    songRequestRow(request)
                                }
                            }
                            .padding(.horizontal, 16)
                        }
                    }
                }
                .padding(.top, 8)
            }
            .navigationTitle("Song Requests")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .preferredColorScheme(.dark)
    }

    private func songRequestRow(_ request: SongRequestManager.Request) -> some View {
        HStack(spacing: 12) {
            // Vote count badge
            VStack(spacing: 2) {
                Text("\(request.votes)")
                    .font(.system(size: 16, weight: .bold, design: .rounded))
                    .foregroundColor(.solunaSol)
                Text("votes")
                    .font(.system(size: 8, weight: .medium))
                    .foregroundColor(.white.opacity(0.3))
            }
            .frame(width: 40)

            // Song info
            VStack(alignment: .leading, spacing: 3) {
                Text(request.title)
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundColor(.white)
                    .lineLimit(1)
                HStack(spacing: 4) {
                    Image(systemName: "person.fill")
                        .font(.system(size: 8))
                        .foregroundColor(.white.opacity(0.3))
                    Text(request.requester)
                        .font(.system(size: 11))
                        .foregroundColor(.white.opacity(0.4))
                    Text("·")
                        .foregroundColor(.white.opacity(0.2))
                    Text(request.timestamp, style: .relative)
                        .font(.system(size: 10))
                        .foregroundColor(.white.opacity(0.3))
                }
            }

            Spacer()

            // Vote button
            Button {
                manager.vote(for: request, via: sendUDP)
                UIImpactFeedbackGenerator(style: .light).impactOccurred()
            } label: {
                Image(systemName: "arrow.up")
                    .font(.system(size: 12, weight: .bold))
                    .foregroundColor(.solunaSol)
                    .frame(width: 32, height: 32)
                    .background(Color.solunaSol.opacity(0.12))
                    .clipShape(Circle())
            }
        }
        .padding(12)
        .background(Color.white.opacity(0.06))
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(
            RoundedRectangle(cornerRadius: 12)
                .stroke(Color.white.opacity(0.06), lineWidth: 0.5)
        )
    }
}
