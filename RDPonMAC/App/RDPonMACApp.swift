import SwiftUI

@main
struct RDPonMACApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @StateObject private var serverManager = RDPServerManager()

    var body: some Scene {
        WindowGroup {
            MainSettingsView()
                .environmentObject(serverManager)
                .frame(minWidth: 500, minHeight: 400)
        }

        MenuBarExtra("RDPonMAC", systemImage: serverManager.isRunning ? "display.and.arrow.down" : "display") {
            StatusBarView()
                .environmentObject(serverManager)
        }
    }
}
