import Foundation
import AppKit

final class PermissionService {

    enum Permission {
        case screenRecording
        case accessibility
    }

    static func checkScreenRecording() -> Bool {
        return CGPreflightScreenCaptureAccess()
    }

    static func requestScreenRecording() -> Bool {
        return CGRequestScreenCaptureAccess()
    }

    static func checkAccessibility() -> Bool {
        return AXIsProcessTrusted()
    }

    static func requestAccessibility() {
        let options = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
        AXIsProcessTrustedWithOptions(options)
    }

    static func openAccessibilitySettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility") {
            NSWorkspace.shared.open(url)
        }
    }

    static func openScreenRecordingSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture") {
            NSWorkspace.shared.open(url)
        }
    }
}
