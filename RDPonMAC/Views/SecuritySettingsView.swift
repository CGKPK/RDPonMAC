import SwiftUI

struct SecuritySettingsView: View {
    @State private var certExists = FileManager.default.fileExists(atPath: CertificateService.defaultCertPath)
    @State private var isRegenerating = false
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 20) {
            Text("Security Settings")
                .font(.title2.bold())

            VStack(alignment: .leading, spacing: 12) {
                HStack {
                    Image(systemName: "lock.shield.fill")
                        .foregroundColor(.accentColor)
                    Text("TLS Certificate")
                        .font(.headline)
                }

                if certExists {
                    HStack {
                        Image(systemName: "checkmark.circle.fill")
                            .foregroundColor(.green)
                        VStack(alignment: .leading) {
                            Text("Certificate installed")
                            Text(CertificateService.defaultCertPath)
                                .font(.caption)
                                .foregroundColor(.secondary)
                                .lineLimit(1)
                                .truncationMode(.middle)
                        }
                    }
                } else {
                    HStack {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .foregroundColor(.yellow)
                        Text("No certificate found. One will be generated on server start.")
                    }
                }

                HStack {
                    Button("Regenerate Certificate") {
                        isRegenerating = true
                        CertificateService.deleteCertificate()
                        if CertificateService.ensureCertificateExists() != nil {
                            certExists = true
                        }
                        isRegenerating = false
                    }
                    .disabled(isRegenerating)

                    if isRegenerating {
                        ProgressView()
                            .scaleEffect(0.7)
                    }
                }

                Divider()

                VStack(alignment: .leading, spacing: 4) {
                    Text("Security Mode: TLS")
                        .font(.subheadline.bold())
                    Text("All connections are encrypted with TLS. Network Level Authentication (NLA) is not yet supported.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
            .padding()

            Spacer()

            HStack {
                Spacer()
                Button("Done") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
            .padding()
        }
        .padding()
    }
}
