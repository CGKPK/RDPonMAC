import Foundation
import AppKit

final class ClipboardService {

    private var lastChangeCount: Int = 0
    private var pollTimer: Timer?
    private var clipboardContext: OpaquePointer? // RDPClipboardContext*

    var onLocalClipboardChanged: (([String]) -> Void)?

    func start() {
        lastChangeCount = NSPasteboard.general.changeCount
        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.checkClipboard()
        }
    }

    func stop() {
        pollTimer?.invalidate()
        pollTimer = nil
    }

    private func checkClipboard() {
        let currentCount = NSPasteboard.general.changeCount
        guard currentCount != lastChangeCount else { return }
        lastChangeCount = currentCount

        var types: [String] = []
        if NSPasteboard.general.string(forType: .string) != nil {
            types.append("text")
        }
        if NSPasteboard.general.data(forType: .rtf) != nil {
            types.append("rtf")
        }
        if NSPasteboard.general.data(forType: .tiff) != nil ||
           NSPasteboard.general.data(forType: .png) != nil {
            types.append("image")
        }

        if !types.isEmpty {
            onLocalClipboardChanged?(types)
        }
    }

    // Read text from local clipboard
    func getLocalText() -> String? {
        return NSPasteboard.general.string(forType: .string)
    }

    // Read image data from local clipboard as BGRA bitmap
    func getLocalImageData() -> Data? {
        guard let data = NSPasteboard.general.data(forType: .tiff) ?? NSPasteboard.general.data(forType: .png) else {
            return nil
        }
        guard let image = NSImage(data: data),
              let cgImage = image.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
            return nil
        }

        let width = cgImage.width
        let height = cgImage.height
        let bytesPerRow = width * 4
        var bitmapData = Data(count: height * bytesPerRow)

        bitmapData.withUnsafeMutableBytes { ptr in
            guard let context = CGContext(
                data: ptr.baseAddress,
                width: width,
                height: height,
                bitsPerComponent: 8,
                bytesPerRow: bytesPerRow,
                space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
            ) else { return }
            context.draw(cgImage, in: CGRect(x: 0, y: 0, width: width, height: height))
        }

        return bitmapData
    }

    // Write text to local clipboard (from remote client)
    func setLocalText(_ text: String) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        lastChangeCount = NSPasteboard.general.changeCount
    }

    // Write image to local clipboard (from remote client)
    func setLocalImage(_ data: Data) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setData(data, forType: .tiff)
        lastChangeCount = NSPasteboard.general.changeCount
    }
}
