import Foundation
import AppKit

/// Polls `NSCursor.current` and pushes shape changes to every connected RDP
/// client via `rdpmac_update_pointer`. Without this the client renders only
/// the system arrow we sent at up_and_running, so resize/I-beam/hand cursors
/// never show — making it hard to see hover affordances (window edges,
/// editable text fields, links, etc.).
///
/// The poller runs on a background queue at ~20 Hz. We compare cursor by
/// image-data hash (cheap, stable) and only push when it changes.
final class CursorService {

    /// Owner provides this so we can route into the C bridge without holding
    /// a Swift reference to RDPServerManager.
    var server: UnsafeMutableRawPointer?

    private var timer: DispatchSourceTimer?
    private let queue = DispatchQueue(label: "com.rdponmac.cursor", qos: .userInteractive)
    private var lastHash: Int = 0

    func start() {
        stop()
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + .milliseconds(50),
                   repeating: .milliseconds(50))
        t.setEventHandler { [weak self] in
            self?.tick()
        }
        t.resume()
        timer = t
    }

    func stop() {
        timer?.cancel()
        timer = nil
        lastHash = 0
    }

    private func tick() {
        guard let server = self.server else { return }

        // NSCursor APIs are main-thread only.
        // `currentSystem` returns the actual cursor the WindowServer is
        // displaying (across all apps and chrome) — including resize
        // cursors at window edges, which `current` does NOT return because
        // those are set by the WindowServer for the focused app's windows
        // and don't propagate to NSCursor.current in unrelated processes.
        var cursor: NSCursor?
        DispatchQueue.main.sync {
            cursor = NSCursor.currentSystem ?? NSCursor.current
        }
        guard let c = cursor else { return }

        let image = c.image
        let hot = c.hotSpot
        let size = image.size

        // Reject zero-sized cursors and anything wildly larger than 96×96
        // (RDP large-pointer cap).
        guard size.width > 0, size.height > 0,
              size.width <= 256, size.height <= 256 else {
            return
        }

        // Render to RGBA. Fixed 32×32 (max supported by older clients) keeps
        // wire payload small and matches the most common cursor size; macOS
        // antialiases the scaled bitmap acceptably.
        let outW = 32
        let outH = 32
        let scaleX = CGFloat(outW) / size.width
        let scaleY = CGFloat(outH) / size.height
        let hotX = UInt32(min(max(0, hot.x * scaleX), CGFloat(outW - 1)))
        let hotY = UInt32(min(max(0, hot.y * scaleY), CGFloat(outH - 1)))

        var rgba = [UInt8](repeating: 0, count: outW * outH * 4)
        let cs = CGColorSpaceCreateDeviceRGB()
        let info = CGImageAlphaInfo.premultipliedLast.rawValue
        guard let ctx = CGContext(
            data: &rgba, width: outW, height: outH,
            bitsPerComponent: 8, bytesPerRow: outW * 4,
            space: cs, bitmapInfo: info) else { return }

        // Convert NSImage → CGImage at the rendered scale.
        var rect = CGRect(x: 0, y: 0, width: CGFloat(outW), height: CGFloat(outH))
        guard let cg = image.cgImage(forProposedRect: &rect,
                                      context: nil, hints: nil) else { return }
        ctx.clear(rect)
        ctx.draw(cg, in: rect)

        // Hash the rendered bytes — cheap dedup so we don't spam the bridge
        // when the cursor is stable.
        var h = 1469598103934665603 as UInt64  // FNV offset
        for byte in rgba {
            h ^= UInt64(byte)
            h &*= 1099511628211      // FNV prime
        }
        let newHash = Int(truncatingIfNeeded: h)
        if newHash == lastHash {
            return
        }
        lastHash = newHash

        rgba.withUnsafeBufferPointer { buf in
            rdpmac_update_pointer(server,
                                  buf.baseAddress!,
                                  UInt32(outW), UInt32(outH),
                                  hotX, hotY)
        }
    }
}
