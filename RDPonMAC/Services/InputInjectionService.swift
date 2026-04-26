import Foundation
import CoreGraphics
import AppKit
import IOKit
import IOKit.hidsystem

final class InputInjectionService {

    private var lastMousePosition = CGPoint.zero
    private var displayBounds: CGRect = .zero
    private var scaleFactor: CGFloat = 1.0
    // RDP-side capture resolution. Mouse events from the client are in this
    // coordinate space; we scale to displayBounds.size before injecting.
    private var remoteWidth: CGFloat = 1920
    private var remoteHeight: CGFloat = 1080

    // IOKit HID connection for login screen input
    private var hidConnection: io_connect_t = 0
    private var hidAvailable = false

    init() {
        openHIDConnection()
    }

    deinit {
        if hidAvailable {
            IOServiceClose(hidConnection)
        }
    }

    private func openHIDConnection() {
        let service = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("IOHIDSystem"))
        guard service != IO_OBJECT_NULL else {
            logToFile("[RDPonMAC] IOKit HID: IOHIDSystem service not found")
            return
        }
        let kr = IOServiceOpen(service, mach_task_self_, UInt32(kIOHIDParamConnectType), &hidConnection)
        IOObjectRelease(service)
        hidAvailable = (kr == KERN_SUCCESS)
        logToFile("[RDPonMAC] IOKit HID: \(hidAvailable ? "available" : "failed (kr=\(kr))")")
    }

    /// Post keyboard via IOKit HID (works at login screen)
    private func postHIDKeyboardEvent(keyCode: UInt8, isDown: Bool) {
        guard hidAvailable else { return }
        var event = NXEventData()
        withUnsafeMutablePointer(to: &event.key.keyCode) { $0.pointee = UInt16(keyCode) }
        let eventType: UInt32 = isDown ? UInt32(NX_KEYDOWN) : UInt32(NX_KEYUP)
        IOHIDPostEvent(hidConnection, eventType, IOGPoint(x: 0, y: 0),
                       &event, UInt32(kNXEventDataVersion), 0, 0)
    }

    /// Post mouse via IOKit HID (works at login screen)
    private func postHIDMouseEvent(eventType: UInt32, point: CGPoint, buttonNumber: UInt8 = 0) {
        guard hidAvailable else { return }
        var event = NXEventData()
        if eventType == UInt32(NX_LMOUSEDOWN) || eventType == UInt32(NX_LMOUSEUP) ||
           eventType == UInt32(NX_RMOUSEDOWN) || eventType == UInt32(NX_RMOUSEUP) {
            withUnsafeMutablePointer(to: &event.mouse.buttonNumber) { $0.pointee = buttonNumber }
        }
        let location = IOGPoint(x: Int16(point.x), y: Int16(point.y))
        IOHIDPostEvent(hidConnection, eventType, location,
                       &event, UInt32(kNXEventDataVersion), 0, 0)
    }

    func configure(displayBounds: CGRect, scaleFactor: CGFloat,
                   remoteWidth: CGFloat = 1920, remoteHeight: CGFloat = 1080) {
        self.displayBounds = displayBounds
        self.scaleFactor = scaleFactor
        self.remoteWidth = remoteWidth
        self.remoteHeight = remoteHeight
    }

    // MARK: - Keyboard Events

    func handleKeyboard(flags: UInt16, scancode: UInt8) {
        let macKeycode = rdp_scancode_to_mac_keycode(scancode, flags)
        guard macKeycode >= 0 else { return }

        let isKeyDown = (flags & UInt16(RDP_KBD_FLAGS_RELEASE)) == 0

        // Try CGEvent first (reliable in user session), IOKit HID as fallback
        if PermissionService.checkAccessibility(),
           let event = CGEvent(keyboardEventSource: nil,
                               virtualKey: CGKeyCode(macKeycode),
                               keyDown: isKeyDown) {
            event.post(tap: .cghidEventTap)
        } else if hidAvailable {
            postHIDKeyboardEvent(keyCode: UInt8(macKeycode), isDown: isKeyDown)
        }
    }

    func handleUnicodeKeyboard(flags: UInt16, code: UInt16) {
        guard PermissionService.checkAccessibility() else { return }

        let isKeyDown = (flags & UInt16(RDP_KBD_FLAGS_RELEASE)) == 0
        guard isKeyDown else { return }

        // For unicode characters, we use key events with the character
        guard let scalar = Unicode.Scalar(code) else { return }
        let char = Character(scalar)
        let str = String(char) as NSString

        guard let event = CGEvent(keyboardEventSource: nil, virtualKey: 0, keyDown: true) else { return }
        event.keyboardSetUnicodeString(stringLength: str.length, unicodeString: Array(str as String).map { $0.utf16.first ?? 0 })
        event.post(tap: .cghidEventTap)

        // Key up
        guard let upEvent = CGEvent(keyboardEventSource: nil, virtualKey: 0, keyDown: false) else { return }
        upEvent.post(tap: .cghidEventTap)
    }

    // MARK: - Mouse Events

    func handleMouse(flags: UInt16, x: UInt16, y: UInt16) {
        // Map (x, y) from the RDP capture coordinate space (remoteWidth x
        // remoteHeight, e.g. 1920x1080) into the macOS display coordinate
        // space (displayBounds.size, typically 1280x720 logical on Retina).
        // Without this scaling, clicks land in the wrong spot whenever the
        // capture resolution differs from the display resolution.
        let scaleX = (remoteWidth  > 0) ? displayBounds.size.width  / remoteWidth  : 1.0
        let scaleY = (remoteHeight > 0) ? displayBounds.size.height / remoteHeight : 1.0
        let screenX = displayBounds.origin.x + CGFloat(x) * scaleX
        let screenY = displayBounds.origin.y + CGFloat(y) * scaleY
        let point = CGPoint(x: screenX, y: screenY)
        let useCGEvent = PermissionService.checkAccessibility()
        let useHID = !useCGEvent && hidAvailable

        if flags & UInt16(RDP_PTR_FLAGS_MOVE) != 0 {
            if useHID {
                postHIDMouseEvent(eventType: UInt32(NX_MOUSEMOVED), point: point)
            } else if useCGEvent {
                postMouseMoveEvent(to: point)
            }
            lastMousePosition = point
        }

        if flags & UInt16(RDP_PTR_FLAGS_BUTTON1) != 0 {
            let isDown = flags & UInt16(RDP_PTR_FLAGS_DOWN) != 0
            if useHID {
                postHIDMouseEvent(eventType: isDown ? UInt32(NX_LMOUSEDOWN) : UInt32(NX_LMOUSEUP), point: point, buttonNumber: 0)
            } else if useCGEvent {
                postMouseButtonEvent(button: .left, isDown: isDown, at: point)
            }
            lastMousePosition = point
        }

        if flags & UInt16(RDP_PTR_FLAGS_BUTTON2) != 0 {
            let isDown = flags & UInt16(RDP_PTR_FLAGS_DOWN) != 0
            if useHID {
                postHIDMouseEvent(eventType: isDown ? UInt32(NX_RMOUSEDOWN) : UInt32(NX_RMOUSEUP), point: point, buttonNumber: 1)
            } else if useCGEvent {
                postMouseButtonEvent(button: .right, isDown: isDown, at: point)
            }
            lastMousePosition = point
        }

        if flags & UInt16(RDP_PTR_FLAGS_BUTTON3) != 0 {
            let isDown = flags & UInt16(RDP_PTR_FLAGS_DOWN) != 0
            if useHID {
                postHIDMouseEvent(eventType: isDown ? UInt32(NX_OMOUSEDOWN) : UInt32(NX_OMOUSEUP), point: point, buttonNumber: 2)
            } else if useCGEvent {
                postMouseButtonEvent(button: .center, isDown: isDown, at: point)
            }
            lastMousePosition = point
        }

        if flags & UInt16(RDP_PTR_FLAGS_WHEEL) != 0 {
            let delta = Int32(flags & 0x01FF)
            let negative = flags & UInt16(RDP_PTR_FLAGS_WHEEL_NEGATIVE) != 0
            let scrollDelta = negative ? -delta : delta
            postScrollEvent(delta: scrollDelta, at: point)
        }
    }

    // MARK: - Event Posting

    private func postMouseMoveEvent(to point: CGPoint) {
        guard let event = CGEvent(mouseEventSource: nil,
                                   mouseType: .mouseMoved,
                                   mouseCursorPosition: point,
                                   mouseButton: .left) else { return }
        event.post(tap: .cghidEventTap)
    }

    private enum MouseButton { case left, right, center }

    private func postMouseButtonEvent(button: MouseButton, isDown: Bool, at point: CGPoint) {
        let (eventType, cgButton): (CGEventType, CGMouseButton)
        switch (button, isDown) {
        case (.left, true):   eventType = .leftMouseDown;  cgButton = .left
        case (.left, false):  eventType = .leftMouseUp;    cgButton = .left
        case (.right, true):  eventType = .rightMouseDown; cgButton = .right
        case (.right, false): eventType = .rightMouseUp;   cgButton = .right
        case (.center, true): eventType = .otherMouseDown; cgButton = .center
        case (.center, false):eventType = .otherMouseUp;   cgButton = .center
        }

        guard let event = CGEvent(mouseEventSource: nil,
                                   mouseType: eventType,
                                   mouseCursorPosition: point,
                                   mouseButton: cgButton) else { return }
        event.post(tap: .cghidEventTap)
    }

    private func postScrollEvent(delta: Int32, at point: CGPoint) {
        guard let event = CGEvent(scrollWheelEvent2Source: nil,
                                   units: .pixel,
                                   wheelCount: 1,
                                   wheel1: delta,
                                   wheel2: 0,
                                   wheel3: 0) else { return }
        event.location = point
        event.post(tap: .cghidEventTap)
    }
}
