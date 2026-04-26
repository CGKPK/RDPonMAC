import Foundation
import AppKit

struct MonitorInfo: Identifiable {
    let id: Int
    let frame: CGRect
    let isPrimary: Bool
    let scaleFactor: CGFloat
    let displayID: CGDirectDisplayID

    var width: Int { Int(frame.width) }
    var height: Int { Int(frame.height) }

    static func enumerate() -> [MonitorInfo] {
        var monitors: [MonitorInfo] = []
        let screens = NSScreen.screens

        for (index, screen) in screens.enumerated() {
            let displayID = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? CGDirectDisplayID ?? 0
            let isPrimary = (index == 0) // Main screen is first in NSScreen.screens
            monitors.append(MonitorInfo(
                id: index,
                frame: screen.frame,
                isPrimary: isPrimary,
                scaleFactor: screen.backingScaleFactor,
                displayID: displayID
            ))
        }

        return monitors
    }
}
