import SwiftUI

struct PermissionsView: View {
    @State private var hasScreenRecording = PermissionService.checkScreenRecording()
    @State private var hasAccessibility = PermissionService.checkAccessibility()
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 20) {
            Text("Required Permissions")
                .font(.title2.bold())

            VStack(spacing: 12) {
                permissionRow(
                    title: "Screen Recording",
                    description: "Required to share your screen with RDP clients",
                    granted: hasScreenRecording,
                    action: {
                        if !hasScreenRecording {
                            PermissionService.openScreenRecordingSettings()
                        }
                    }
                )

                permissionRow(
                    title: "Accessibility",
                    description: "Required for keyboard and mouse input from RDP clients",
                    granted: hasAccessibility,
                    action: {
                        if !hasAccessibility {
                            PermissionService.requestAccessibility()
                        }
                    }
                )
            }
            .padding()

            Text("After granting permissions, you may need to restart the app.")
                .font(.caption)
                .foregroundColor(.secondary)

            HStack {
                Button("Refresh") {
                    hasScreenRecording = PermissionService.checkScreenRecording()
                    hasAccessibility = PermissionService.checkAccessibility()
                }
                Spacer()
                Button("Done") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
            .padding()
        }
        .padding()
    }

    private func permissionRow(title: String, description: String, granted: Bool, action: @escaping () -> Void) -> some View {
        HStack {
            Image(systemName: granted ? "checkmark.circle.fill" : "xmark.circle.fill")
                .foregroundColor(granted ? .green : .red)
                .font(.title2)

            VStack(alignment: .leading) {
                Text(title).font(.headline)
                Text(description).font(.caption).foregroundColor(.secondary)
            }

            Spacer()

            if !granted {
                Button("Grant") { action() }
                    .buttonStyle(.bordered)
            }
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 8).fill(Color(nsColor: .controlBackgroundColor)))
    }
}
