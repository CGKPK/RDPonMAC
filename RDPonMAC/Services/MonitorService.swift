import Foundation
import AppKit

final class MonitorService {

    static func getMonitors() -> [MonitorInfo] {
        return MonitorInfo.enumerate()
    }

    static func getPrimaryMonitor() -> MonitorInfo? {
        return MonitorInfo.enumerate().first(where: { $0.isPrimary })
    }

    static func getDisplayBounds(for monitor: MonitorInfo) -> CGRect {
        return CGDisplayBounds(monitor.displayID)
    }
}
