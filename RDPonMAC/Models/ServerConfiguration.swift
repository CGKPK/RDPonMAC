import Foundation

struct ServerConfiguration: Codable {
    var port: UInt32 = 3389
    var bindAddress: String = "0.0.0.0"
    var authentication: Bool = false
    var frameRate: Int = 30
    var certFile: String = ""
    var keyFile: String = ""

    static let defaultConfigURL: URL = {
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let dir = appSupport.appendingPathComponent("RDPonMAC", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("config.json")
    }()

    static func load() -> ServerConfiguration {
        guard let data = try? Data(contentsOf: defaultConfigURL),
              let config = try? JSONDecoder().decode(ServerConfiguration.self, from: data) else {
            return ServerConfiguration()
        }
        return config
    }

    func save() {
        guard let data = try? JSONEncoder().encode(self) else { return }
        try? data.write(to: Self.defaultConfigURL)
    }
}
