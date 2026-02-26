import SwiftUI

@main
struct SolunaControlApp: App {
    @StateObject private var daemon = DaemonClient()

    var body: some Scene {
        MenuBarExtra("Soluna", systemImage: "waveform") {
            MenuBarView(daemon: daemon)
                .frame(width: 280)
        }
        .menuBarExtraStyle(.window)
    }
}
