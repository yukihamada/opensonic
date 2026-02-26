//
//  SolunaControlApp.swift
//  SolunaControl — macOS menu bar app for solunad
//

import SwiftUI

@main
struct SolunaControlApp: App {
    @StateObject private var daemon = DaemonClient()

    var body: some Scene {
        MenuBarExtra {
            MenuContent()
                .environmentObject(daemon)
        } label: {
            MenuBarIcon(daemon: daemon)
        }
        .menuBarExtraStyle(.window)
    }
}

// Animated menu bar icon
struct MenuBarIcon: View {
    @ObservedObject var daemon: DaemonClient

    var body: some View {
        HStack(spacing: 3) {
            Image(systemName: "waveform")
            if daemon.monitorRunning {
                Circle()
                    .fill(.green)
                    .frame(width: 5, height: 5)
            }
        }
    }
}
