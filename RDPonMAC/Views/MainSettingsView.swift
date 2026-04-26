import SwiftUI

struct MainSettingsView: View {
    @EnvironmentObject var serverManager: RDPServerManager
    @State private var config = ServerConfiguration.load()
    @State private var showingPermissions = false
    @State private var showingSecurity = false

    var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Image(systemName: "desktopcomputer")
                    .font(.largeTitle)
                    .foregroundColor(.accentColor)
                VStack(alignment: .leading) {
                    Text("RDPonMAC Server")
                        .font(.title2.bold())
                    Text(serverManager.statusMessage)
                        .font(.caption)
                        .foregroundColor(serverManager.isRunning ? .green : .secondary)
                }
                Spacer()
                serverToggle
            }
            .padding()

            Divider()

            // Settings
            Form {
                Section("Network") {
                    HStack {
                        Text("Port")
                        Spacer()
                        TextField("Port", value: $config.port, format: .number)
                            .frame(width: 80)
                            .textFieldStyle(.roundedBorder)
                            .disabled(serverManager.isRunning)
                    }
                    HStack {
                        Text("Bind Address")
                        Spacer()
                        TextField("0.0.0.0", text: $config.bindAddress)
                            .frame(width: 150)
                            .textFieldStyle(.roundedBorder)
                            .disabled(serverManager.isRunning)
                    }
                }

                Section("Performance") {
                    HStack {
                        Text("Frame Rate")
                        Spacer()
                        Picker("", selection: $config.frameRate) {
                            Text("15 FPS").tag(15)
                            Text("30 FPS").tag(30)
                            Text("60 FPS").tag(60)
                        }
                        .frame(width: 120)
                        .disabled(serverManager.isRunning)
                    }
                }

                Section("Connected Clients (\(serverManager.connectedClients.count))") {
                    if serverManager.connectedClients.isEmpty {
                        Text("No clients connected")
                            .foregroundColor(.secondary)
                            .italic()
                    } else {
                        ForEach(serverManager.connectedClients) { client in
                            HStack {
                                Image(systemName: "person.fill")
                                Text(client.clientAddress)
                                Spacer()
                                Text(client.connectedAt, style: .relative)
                                    .foregroundColor(.secondary)
                            }
                        }
                    }
                }
            }
            .formStyle(.grouped)

            // Bottom bar
            HStack {
                Button("Permissions...") {
                    showingPermissions = true
                }
                Button("Security...") {
                    showingSecurity = true
                }
                Spacer()
                if let error = serverManager.lastError {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundColor(.yellow)
                    Text(error)
                        .font(.caption)
                        .foregroundColor(.red)
                        .lineLimit(1)
                }
            }
            .padding()
        }
        .sheet(isPresented: $showingPermissions) {
            PermissionsView()
                .frame(width: 400, height: 300)
        }
        .sheet(isPresented: $showingSecurity) {
            SecuritySettingsView()
                .frame(width: 400, height: 300)
        }
        .onChange(of: config.port) { _, _ in config.save() }
        .onChange(of: config.bindAddress) { _, _ in config.save() }
        .onChange(of: config.frameRate) { _, _ in config.save() }
    }

    private var serverToggle: some View {
        Button(action: {
            Task {
                if serverManager.isRunning {
                    await serverManager.stop()
                } else {
                    await serverManager.start()
                }
            }
        }) {
            HStack {
                Image(systemName: serverManager.isRunning ? "stop.fill" : "play.fill")
                Text(serverManager.isRunning ? "Stop" : "Start")
            }
            .frame(width: 80)
        }
        .buttonStyle(.borderedProminent)
        .tint(serverManager.isRunning ? .red : .green)
    }
}
