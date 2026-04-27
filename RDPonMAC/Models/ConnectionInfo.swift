import Foundation

struct ConnectionInfo: Identifiable {
    let id: String = Foundation.UUID().uuidString
    let clientAddress: String
    let connectedAt: Date
    var username: String?
    /// What the client window asked for over DYNVC Display Control. Live
    /// updates as the user drags the mstsc window border.
    var width: UInt32?
    var height: UInt32?
    /// The macOS display mode we actually switched into to satisfy that
    /// request. Usually a few pixels off (Apple exposes a fixed set of
    /// modes), and SCStream handles the small final scale.
    var modeWidth: UInt32?
    var modeHeight: UInt32?
}
