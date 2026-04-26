import Foundation
import ScreenCaptureKit
import CoreVideo
import CoreMedia

struct CaptureRect {
    var left: UInt16
    var top: UInt16
    var right: UInt16
    var bottom: UInt16
}

protocol ScreenCaptureDelegate: AnyObject {
    func screenCaptureDidOutputFrame(_ data: UnsafePointer<UInt8>, width: UInt32, height: UInt32, stride: UInt32, dirtyRects: [CaptureRect])
    func screenCaptureDidFail(_ error: Error)
}

final class ScreenCaptureService: NSObject {

    weak var delegate: ScreenCaptureDelegate?

    private var stream: SCStream?
    private var isCapturing = false
    private let captureQueue = DispatchQueue(label: "com.rdponmac.screencapture", qos: .userInteractive)
    private var previousFrameData: Data?
    private var frameRate: Int = 30

    // CGDisplayCreateImage fallback when ScreenCaptureKit isn't available
    private var fallbackTimer: DispatchSourceTimer?
    private var usingFallback = false

    func startCapture(displayID: CGDirectDisplayID? = nil, frameRate: Int = 30) async throws {
        guard !isCapturing else { return }
        self.frameRate = frameRate

        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)

        // Find the target display
        let targetDisplay: SCDisplay
        if let displayID = displayID, let display = content.displays.first(where: { $0.displayID == displayID }) {
            targetDisplay = display
        } else {
            guard let mainDisplay = content.displays.first else {
                throw ScreenCaptureError.noDisplayFound
            }
            targetDisplay = mainDisplay
        }

        // Configure stream — use logical resolution (not Retina physical pixels)
        // This reduces data by 4x on Retina displays (e.g., 1280x720 instead of 2560x1440)
        let scaleFactor = NSScreen.main?.backingScaleFactor ?? 2.0
        let captureWidth = Int(CGFloat(targetDisplay.width) / scaleFactor)
        let captureHeight = Int(CGFloat(targetDisplay.height) / scaleFactor)

        let config = SCStreamConfiguration()
        config.width = captureWidth
        config.height = captureHeight
        config.pixelFormat = kCVPixelFormatType_32BGRA
        config.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(frameRate))
        config.showsCursor = false
        config.queueDepth = 3

        // Audio capture configuration
        config.capturesAudio = true
        config.sampleRate = 48000
        config.channelCount = 2

        // Create filter for the target display
        let filter = SCContentFilter(display: targetDisplay, excludingWindows: [])

        // Create and start stream
        let stream = SCStream(filter: filter, configuration: config, delegate: self)
        try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: captureQueue)
        try stream.addStreamOutput(self, type: .audio, sampleHandlerQueue: captureQueue)
        try await stream.startCapture()

        self.stream = stream
        self.isCapturing = true
    }

    // MARK: - CGDisplayCreateImage fallback

    func startFallbackCapture(displayID: CGDirectDisplayID = CGMainDisplayID(), frameRate: Int = 10) {
        guard !usingFallback else { return }
        usingFallback = true
        isCapturing = true

        let interval = 1.0 / Double(max(1, frameRate))
        let timer = DispatchSource.makeTimerSource(queue: captureQueue)
        timer.schedule(deadline: .now(), repeating: interval)
        timer.setEventHandler { [weak self] in
            self?.captureWithCGDisplay(displayID)
        }
        timer.resume()
        fallbackTimer = timer
    }

    private func captureWithCGDisplay(_ displayID: CGDirectDisplayID) {
        guard let cgImage = CGDisplayCreateImage(displayID) else { return }

        let width = UInt32(cgImage.width)
        let height = UInt32(cgImage.height)
        let stride = width * 4
        let dataSize = Int(height * stride)

        // Render CGImage into a BGRA pixel buffer
        guard let context = CGContext(
            data: nil,
            width: Int(width),
            height: Int(height),
            bitsPerComponent: 8,
            bytesPerRow: Int(stride),
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue  // BGRA
        ) else { return }

        context.draw(cgImage, in: CGRect(x: 0, y: 0, width: Int(width), height: Int(height)))

        guard let data = context.data else { return }
        let ptr = data.assumingMemoryBound(to: UInt8.self)
        delegate?.screenCaptureDidOutputFrame(ptr, width: width, height: height, stride: stride, dirtyRects: [])
    }

    func stopCapture() async {
        if usingFallback {
            fallbackTimer?.cancel()
            fallbackTimer = nil
            usingFallback = false
            isCapturing = false
            return
        }

        guard isCapturing, let stream = stream else { return }

        try? await stream.stopCapture()
        self.stream = nil
        self.isCapturing = false
        self.previousFrameData = nil
    }

    var capturing: Bool { isCapturing }
}

// MARK: - SCStreamOutput

extension ScreenCaptureService: SCStreamOutput {

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        switch type {
        case .screen:
            handleVideoFrame(sampleBuffer)
        case .audio:
            handleAudioFrame(sampleBuffer)
        @unknown default:
            break
        }
    }

    private func handleVideoFrame(_ sampleBuffer: CMSampleBuffer) {
        guard let pixelBuffer = sampleBuffer.imageBuffer else { return }

        // Extract dirty rects from ScreenCaptureKit frame info
        var dirtyRects: [CaptureRect] = []
        if let attachmentsArray = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [NSDictionary],
           let attachment = attachmentsArray.first {
            // Check frame status — skip idle frames
            if let status = attachment[SCStreamFrameInfo.status] as? Int,
               status != SCFrameStatus.complete.rawValue {
                return  // Frame not complete, skip
            }
            // Get dirty rects
            if let rawRects = attachment[SCStreamFrameInfo.dirtyRects] as? [NSDictionary] {
                for r in rawRects {
                    if let x = r["X"] as? CGFloat, let y = r["Y"] as? CGFloat,
                       let w = r["Width"] as? CGFloat, let h = r["Height"] as? CGFloat {
                        dirtyRects.append(CaptureRect(
                            left: UInt16(max(0, x)),
                            top: UInt16(max(0, y)),
                            right: UInt16(min(x + w, 65535)),
                            bottom: UInt16(min(y + h, 65535))
                        ))
                    }
                }
            } else if let rawRects = attachment[SCStreamFrameInfo.dirtyRects] as? [CGRect] {
                for r in rawRects {
                    dirtyRects.append(CaptureRect(
                        left: UInt16(max(0, r.origin.x)),
                        top: UInt16(max(0, r.origin.y)),
                        right: UInt16(min(r.origin.x + r.size.width, 65535)),
                        bottom: UInt16(min(r.origin.y + r.size.height, 65535))
                    ))
                }
            }
        }
        // empty dirtyRects = full screen update in our C code

        CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }

        guard let baseAddress = CVPixelBufferGetBaseAddress(pixelBuffer) else { return }

        let width = UInt32(CVPixelBufferGetWidth(pixelBuffer))
        let height = UInt32(CVPixelBufferGetHeight(pixelBuffer))
        let stride = UInt32(CVPixelBufferGetBytesPerRow(pixelBuffer))

        let ptr = baseAddress.assumingMemoryBound(to: UInt8.self)
        delegate?.screenCaptureDidOutputFrame(ptr, width: width, height: height, stride: stride, dirtyRects: dirtyRects)
    }

    private func handleAudioFrame(_ sampleBuffer: CMSampleBuffer) {
        // Audio frames are handled by AudioCaptureService via a separate path
        // The audio data from ScreenCaptureKit is forwarded through NotificationCenter
        NotificationCenter.default.post(
            name: .screenCaptureAudioFrame,
            object: nil,
            userInfo: ["sampleBuffer": sampleBuffer]
        )
    }
}

// MARK: - SCStreamDelegate

extension ScreenCaptureService: SCStreamDelegate {

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        isCapturing = false
        delegate?.screenCaptureDidFail(error)
    }
}

// MARK: - Errors and Notifications

enum ScreenCaptureError: LocalizedError {
    case noDisplayFound
    case captureNotSupported

    var errorDescription: String? {
        switch self {
        case .noDisplayFound:
            return "No display found for screen capture"
        case .captureNotSupported:
            return "Screen capture is not supported on this system"
        }
    }
}

extension Notification.Name {
    static let screenCaptureAudioFrame = Notification.Name("screenCaptureAudioFrame")
}
