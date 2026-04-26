import Foundation

struct ConnectionInfo: Identifiable {
    let id: String = Foundation.UUID().uuidString
    let clientAddress: String
    let connectedAt: Date
    var username: String?
}
