import SwiftUI

struct StatusBarView: View {
    @EnvironmentObject var serverManager: RDPServerManager

    var body: some View {
        VStack(alignment: .leading) {
            HStack {
                Circle()
                    .fill(serverManager.isRunning ? .green : .red)
                    .frame(width: 8, height: 8)
                Text(serverManager.isRunning ? "Server Running" : "Server Stopped")
                    .font(.headline)
            }

            if serverManager.isRunning {
                Text("\(serverManager.connectedClients.count) client(s) connected")
                    .font(.caption)
            }

            Divider()

            Button(serverManager.isRunning ? "Stop Server" : "Start Server") {
                Task {
                    if serverManager.isRunning {
                        await serverManager.stop()
                    } else {
                        await serverManager.start()
                    }
                }
            }

            Divider()

            Button("Quit RDPonMAC") {
                Task {
                    if serverManager.isRunning {
                        await serverManager.stop()
                    }
                    NSApplication.shared.terminate(nil)
                }
            }
            .keyboardShortcut("q")
        }
    }
}
