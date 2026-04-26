import AppKit

class AppDelegate: NSObject, NSApplicationDelegate {

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Request screen recording permission once on first launch only.
        // CGRequestScreenCaptureAccess() opens System Preferences every time it's called,
        // so we gate it with a UserDefaults flag.
        let key = "HasRequestedScreenRecording"
        if !UserDefaults.standard.bool(forKey: key) {
            _ = PermissionService.requestScreenRecording()
            UserDefaults.standard.set(true, forKey: key)
        }
        // Also request accessibility once
        if !PermissionService.checkAccessibility() {
            PermissionService.requestAccessibility()
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return false // Keep running in menu bar
    }
}
