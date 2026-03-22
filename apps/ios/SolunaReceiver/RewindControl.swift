//
//  RewindControl.swift
//  SolunaReceiver
//
//  Rewind button — sends REPLAY:30 to relay for 30-second replay buffer.
//

import SwiftUI

struct RewindButton: View {
    let sendUDP: (String) -> Void
    @State private var rewinding = false

    var body: some View {
        Button {
            sendUDP("REPLAY:30\n")
            rewinding = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { rewinding = false }
            UIImpactFeedbackGenerator(style: .medium).impactOccurred()
        } label: {
            Image(systemName: "gobackward.30")
                .font(.system(size: 14))
                .foregroundColor(rewinding ? .solunaSol : .white.opacity(0.4))
                .scaleEffect(rewinding ? 1.3 : 1.0)
                .animation(.spring(response: 0.3), value: rewinding)
        }
    }
}
