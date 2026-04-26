import Foundation
import Combine
import CoreGraphics
import AppKit
import os.log

private let logger = Logger(subsystem: "com.rdponmac.app", category: "server")

func logToFile(_ msg: String) {
    let line = "\(Date()): \(msg)\n"
    let path = "/tmp/rdponmac.log"
    if let fh = FileHandle(forWritingAtPath: path) {
        fh.seekToEndOfFile()
        fh.write(line.data(using: .utf8)!)
        fh.closeFile()
    } else {
        FileManager.default.createFile(atPath: path, contents: line.data(using: .utf8))
    }
    logger.info("\(msg)")
}

@MainActor
final class RDPServerManager: ObservableObject {

    @Published var isRunning = false
    @Published var statusMessage = "Stopped"
    @Published var connectedClients: [ConnectionInfo] = []
    @Published var lastError: String?

    private var server: RDPServerHandle?
    // Thread-safe copy for nonisolated screen capture callback
    nonisolated(unsafe) var _nonisolatedServer: RDPServerHandle?
    private let screenCapture = ScreenCaptureService()
    private let inputInjection = InputInjectionService()
    private let clipboardService = ClipboardService()
    private let audioCaptureService = AudioCaptureService()
    private var config = ServerConfiguration.load()

    // Retained reference for C callback context
    private var callbackBridge: Unmanaged<RDPServerManager>?

    func start() async {
        guard !isRunning else { return }

        // Kill any stale process holding our port from a previous run
        killProcessOnPort(config.port)

        // Ensure certificates exist
        guard let certPaths = CertificateService.ensureCertificateExists() else {
            lastError = "Failed to generate TLS certificate"
            return
        }

        config = ServerConfiguration.load()
        if config.certFile.isEmpty { config.certFile = certPaths.certFile }
        if config.keyFile.isEmpty { config.keyFile = certPaths.keyFile }

        // Set up Swift callback bridge before creating server
        setupCallbacks()

        // Create server on background thread
        let certFile = config.certFile
        let keyFile = config.keyFile
        let port = config.port
        let auth = config.authentication

        logToFile("[RDPonMAC] Creating server...")
        let result: RDPServerHandle? = await Task.detached {
            guard let server = rdp_server_create() else {
                logToFile("[RDPonMAC] rdp_server_create() failed")
                return nil
            }

            logToFile("[RDPonMAC] Configuring: port=\(port) cert=\(certFile) key=\(keyFile)")
            let configured = rdp_server_configure(
                server,
                port,
                certFile,
                keyFile,
                auth
            )
            guard configured else {
                logToFile("[RDPonMAC] rdp_server_configure() failed")
                rdp_server_free(server)
                return nil
            }

            return server
        }.value

        guard let serverPtr = result else {
            lastError = "Failed to create RDP server"
            releaseCallbackBridge()
            return
        }

        self.server = serverPtr
        self._nonisolatedServer = serverPtr

        // Init and start on background thread
        logToFile("[RDPonMAC] Initializing and starting server...")
        let startResult = await Task.detached { () -> Int32 in
            let initStatus = rdp_server_init(serverPtr)
            logToFile("[RDPonMAC] shadow_server_init returned: \(initStatus)")
            guard initStatus >= 0 else { return Int32(initStatus) }
            let startStatus = rdp_server_start(serverPtr)
            logToFile("[RDPonMAC] shadow_server_start returned: \(startStatus)")
            return Int32(startStatus)
        }.value

        if startResult < 0 {
            lastError = "Failed to start server (error: \(startResult))"
            logToFile("[RDPonMAC] Server failed to start: \(startResult)")
            await cleanup()
            return
        }
        logToFile("[RDPonMAC] Server started successfully on port \(config.port)")

        // Check if surface was created by shadow_server_init
        if let server = self.server {
            let surfaceOK = rdpmac_check_surface(server)
            logToFile("[RDPonMAC] Surface check after init: \(surfaceOK)")
        }

        // Initialize surface with a blue placeholder frame so clients can connect
        // even before screen capture starts
        if let server = self.server {
            // Match the capture resolution we configure in ScreenCaptureService:
            // native pixels of the primary display. The tile-based delivery
            // path in RDPxRDPListener handles the PDU budget, so we no longer
            // need to clamp to 1920×1080.
            let mainDisplay = CGMainDisplayID()
            let pw = UInt32(CGDisplayPixelsWide(mainDisplay))
            let ph = UInt32(CGDisplayPixelsHigh(mainDisplay))
            let pstride = pw * 4
            var blueFrame = [UInt8](repeating: 0, count: Int(ph * pstride))
            // Fill with dark blue (BGRX format)
            var idx = 0
            while idx < blueFrame.count {
                blueFrame[idx] = 0x80     // B
                blueFrame[idx+1] = 0x40   // G
                blueFrame[idx+2] = 0x20   // R
                blueFrame[idx+3] = 0xFF   // X
                idx += 4
            }
            blueFrame.withUnsafeBufferPointer { ptr in
                rdpmac_update_frame(server, ptr.baseAddress!, pw, ph, pstride, 0, nil)
            }
            rdpmac_notify_update(server)
            logToFile("[RDPonMAC] Initialized placeholder frame \(pw)x\(ph)")
        }

        // Start screen capture — try ScreenCaptureKit first, fall back to CGDisplayCreateImage
        do {
            logToFile("[RDPonMAC] Starting screen capture (ScreenCaptureKit)...")
            screenCapture.delegate = self
            try await screenCapture.startCapture(frameRate: config.frameRate)
            logToFile("[RDPonMAC] Screen capture started (ScreenCaptureKit)")
        } catch {
            logToFile("[RDPonMAC] ScreenCaptureKit failed: \(error)")
            logToFile("[RDPonMAC] Falling back to CGDisplayCreateImage...")
            screenCapture.delegate = self
            screenCapture.startFallbackCapture(frameRate: min(config.frameRate, 10))
            logToFile("[RDPonMAC] Fallback capture started (CGDisplayCreateImage)")
        }

        // Start clipboard monitoring
        clipboardService.start()
        clipboardService.onLocalClipboardChanged = { [weak self] types in
            self?.handleLocalClipboardChanged(types)
        }

        // Start audio capture
        audioCaptureService.start()

        // Configure input injection for primary monitor. Tell the input
        // service the same RDP coordinate space we capture in (native pixels)
        // so its remote→display scaling matches.
        if let primary = MonitorService.getPrimaryMonitor() {
            let mainDisplay = CGMainDisplayID()
            let nativeW = CGFloat(CGDisplayPixelsWide(mainDisplay))
            let nativeH = CGFloat(CGDisplayPixelsHigh(mainDisplay))
            inputInjection.configure(
                displayBounds: MonitorService.getDisplayBounds(for: primary),
                scaleFactor: primary.scaleFactor,
                remoteWidth: nativeW,
                remoteHeight: nativeH
            )
        }

        isRunning = true
        statusMessage = "Running on port \(config.port)"
        lastError = nil
    }

    func stop() async {
        guard isRunning else { return }

        await screenCapture.stopCapture()
        clipboardService.stop()
        audioCaptureService.stop()

        await cleanup()

        isRunning = false
        statusMessage = "Stopped"
        connectedClients = []
    }

    private func cleanup() async {
        guard let server = self.server else { return }

        await Task.detached {
            rdp_server_stop(server)
            rdp_server_uninit(server)
            rdp_server_free(server)
        }.value

        self.server = nil
        self._nonisolatedServer = nil
        releaseCallbackBridge()
    }

    // MARK: - Port cleanup

    private func killProcessOnPort(_ port: UInt32) {
        let task = Process()
        task.launchPath = "/usr/sbin/lsof"
        task.arguments = ["-ti:\(port)"]
        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = FileHandle.nullDevice
        do {
            try task.run()
            task.waitUntilExit()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            if let output = String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines),
               !output.isEmpty {
                // Kill each PID found (skip our own PID)
                let myPid = ProcessInfo.processInfo.processIdentifier
                for line in output.split(separator: "\n") {
                    if let pid = Int32(line.trimmingCharacters(in: .whitespaces)), pid != myPid {
                        logToFile("[RDPonMAC] Killing stale process \(pid) on port \(port)")
                        kill(pid, SIGKILL)
                    }
                }
                // Brief wait for port to be released
                Thread.sleep(forTimeInterval: 0.5)
            }
        } catch {
            logToFile("[RDPonMAC] Port cleanup failed: \(error)")
        }
    }

    // MARK: - C Callback Setup

    private func setupCallbacks() {
        callbackBridge = Unmanaged.passRetained(self)
        let ctx = callbackBridge!.toOpaque()

        g_macSubsystemContext.swiftContext = ctx

        g_macSubsystemContext.onCaptureStart = { _ in }
        g_macSubsystemContext.onCaptureStop = { _ in }

        g_macSubsystemContext.onKeyboardEvent = { ctx, flags, code in
            guard let ctx = ctx else { return }
            let manager = Unmanaged<RDPServerManager>.fromOpaque(ctx).takeUnretainedValue()
            manager.inputInjection.handleKeyboard(flags: flags, scancode: code)
        }

        g_macSubsystemContext.onUnicodeKeyboardEvent = { ctx, flags, code in
            guard let ctx = ctx else { return }
            let manager = Unmanaged<RDPServerManager>.fromOpaque(ctx).takeUnretainedValue()
            manager.inputInjection.handleUnicodeKeyboard(flags: flags, code: code)
        }

        g_macSubsystemContext.onMouseEvent = { ctx, flags, x, y in
            guard let ctx = ctx else { return }
            let manager = Unmanaged<RDPServerManager>.fromOpaque(ctx).takeUnretainedValue()
            manager.inputInjection.handleMouse(flags: flags, x: x, y: y)
        }

        g_macSubsystemContext.onClientConnect = { ctx in
            guard let ctx = ctx else { return }
            let manager = Unmanaged<RDPServerManager>.fromOpaque(ctx).takeUnretainedValue()
            DispatchQueue.main.async {
                let info = ConnectionInfo(clientAddress: "unknown", connectedAt: Date())
                manager.connectedClients.append(info)
            }
        }

        g_macSubsystemContext.onClientDisconnect = { ctx in
            guard let ctx = ctx else { return }
            let manager = Unmanaged<RDPServerManager>.fromOpaque(ctx).takeUnretainedValue()
            DispatchQueue.main.async {
                if !manager.connectedClients.isEmpty {
                    manager.connectedClients.removeLast()
                }
            }
        }
    }

    private func releaseCallbackBridge() {
        g_macSubsystemContext.swiftContext = nil
        g_macSubsystemContext.onCaptureStart = nil
        g_macSubsystemContext.onCaptureStop = nil
        g_macSubsystemContext.onKeyboardEvent = nil
        g_macSubsystemContext.onUnicodeKeyboardEvent = nil
        g_macSubsystemContext.onMouseEvent = nil
        g_macSubsystemContext.onClientConnect = nil
        g_macSubsystemContext.onClientDisconnect = nil

        callbackBridge?.release()
        callbackBridge = nil
    }

    // MARK: - Clipboard

    private nonisolated func handleLocalClipboardChanged(_ types: [String]) {
        // Notify connected RDP clients of new clipboard content
    }
}

// MARK: - ScreenCaptureDelegate

extension RDPServerManager: ScreenCaptureDelegate {

    nonisolated func screenCaptureDidOutputFrame(_ data: UnsafePointer<UInt8>, width: UInt32, height: UInt32, stride: UInt32, dirtyRects: [CaptureRect]) {
        struct FrameCounter { static var count = 0 }
        guard let swiftCtx = g_macSubsystemContext.swiftContext else { return }
        let manager = Unmanaged<RDPServerManager>.fromOpaque(swiftCtx).takeUnretainedValue()
        guard let server = manager._nonisolatedServer else { return }

        if FrameCounter.count == 0 {
            let surfaceOK = rdpmac_check_surface(server)
            logToFile("[RDPonMAC] First capture frame: \(width)x\(height) stride=\(stride) surfaceOK=\(surfaceOK) dirtyRects=\(dirtyRects.count)")
        }
        FrameCounter.count += 1

        if dirtyRects.isEmpty {
            // No dirty rect info — mark full screen
            rdpmac_update_frame(server, data, width, height, stride, 0, nil)
        } else {
            // Pass dirty rects so only changed regions are encoded
            dirtyRects.withUnsafeBufferPointer { rectsPtr in
                // CaptureRect has the same layout as RDPRect (4x UInt16)
                rectsPtr.baseAddress!.withMemoryRebound(to: RDPRect.self, capacity: dirtyRects.count) { rdpRects in
                    rdpmac_update_frame(server, data, width, height, stride, UInt32(dirtyRects.count), rdpRects)
                }
            }
        }
        rdpmac_notify_update(server)
    }

    nonisolated func screenCaptureDidFail(_ error: Error) {
        DispatchQueue.main.async { [weak self] in
            self?.lastError = "Screen capture error: \(error.localizedDescription)"
            self?.isRunning = false
            self?.statusMessage = "Error"
        }
    }
}
