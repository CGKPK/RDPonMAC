import Foundation

final class CertificateService {

    struct CertPaths {
        let certFile: String
        let keyFile: String
    }

    static let certDirectory: URL = {
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let dir = appSupport.appendingPathComponent("RDPonMAC/certs", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }()

    static var defaultCertPath: String {
        certDirectory.appendingPathComponent("server.crt").path
    }

    static var defaultKeyPath: String {
        certDirectory.appendingPathComponent("server.key").path
    }

    static func ensureCertificateExists() -> CertPaths? {
        let certPath = defaultCertPath
        let keyPath = defaultKeyPath

        if FileManager.default.fileExists(atPath: certPath) &&
           FileManager.default.fileExists(atPath: keyPath) {
            return CertPaths(certFile: certPath, keyFile: keyPath)
        }

        return generateSelfSignedCertificate(certPath: certPath, keyPath: keyPath)
    }

    static func generateSelfSignedCertificate(certPath: String, keyPath: String) -> CertPaths? {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/openssl")
        process.arguments = [
            "req", "-x509", "-newkey", "rsa:2048",
            "-keyout", keyPath,
            "-out", certPath,
            "-days", "365",
            "-nodes",
            "-subj", "/CN=RDPonMAC"
        ]

        let pipe = Pipe()
        process.standardError = pipe
        process.standardOutput = pipe

        do {
            try process.run()
            process.waitUntilExit()

            if process.terminationStatus == 0 {
                return CertPaths(certFile: certPath, keyFile: keyPath)
            }
        } catch {
            print("Failed to generate certificate: \(error)")
        }

        return nil
    }

    static func deleteCertificate() {
        try? FileManager.default.removeItem(atPath: defaultCertPath)
        try? FileManager.default.removeItem(atPath: defaultKeyPath)
    }
}
