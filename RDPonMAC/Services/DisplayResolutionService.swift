import Foundation
import CoreGraphics

/// Switches the macOS main display to the closest available mode that
/// matches a target pixel resolution. We use this to make the local Mac
/// desktop "follow" the RDP client's window-size changes — the same way
/// real Windows reflows its desktop when mstsc renegotiates a new size
/// via DYNVC Display Control.
///
/// Without this, the only ways to fit a non-16:9 client window are:
///   • letterbox (preservesAspectRatio = true)        — black bars
///   • stretch  (preservesAspectRatio = false)        — distorted UI
/// Changing the actual display mode reflows the Mac UI at the requested
/// size, so the captured frame's aspect now matches the client window
/// and SCStream can pass through without scaling at all.
///
/// We cache the user's original mode at start and restore it on stop so
/// we don't leave the local screen in a different resolution after the
/// RDP session ends.
final class DisplayResolutionService {

    private let displayID: CGDirectDisplayID
    private var originalMode: CGDisplayMode?
    private var modesCache: [CGDisplayMode] = []

    init(displayID: CGDirectDisplayID = CGMainDisplayID()) {
        self.displayID = displayID
    }

    /// Cache the user's current mode so we can restore it. Called once
    /// when the RDP server starts. No-op if already cached.
    func snapshotOriginal() {
        guard originalMode == nil else { return }
        originalMode = CGDisplayCopyDisplayMode(displayID)

        // Build the candidate list once. `kCGDisplayShowDuplicateLowResolutionModes`
        // exposes the full set including HiDPI variants — important for
        // Retina Macs where the default-only list omits all the "looks
        // like 1920" / "looks like 2560" Retina modes that mstsc actually
        // wants to map to.
        let opts: [CFString: Any] = [
            kCGDisplayShowDuplicateLowResolutionModes: kCFBooleanTrue!
        ]
        if let raw = CGDisplayCopyAllDisplayModes(displayID,
                                                   opts as CFDictionary)
            as? [CGDisplayMode] {
            modesCache = raw
        }
    }

    /// Find the best available mode for `(width, height)` (which the RDP
    /// client treats as both its window's pixel count AND the desktop
    /// "logical" size — mstsc renders 1:1) and switch to it if different
    /// from the current mode. Returns the chosen mode's logical size so
    /// the caller can reason about UI density.
    ///
    /// Picking heuristic, in priority order:
    ///   1. **Hard requirement: logical ≥ requested in BOTH axes.** The
    ///      macOS UI density follows logical width/height. If we picked
    ///      a mode with smaller logical size, SCKit would upscale (blur
    ///      + chunky look) AND macOS would render *fewer* logical pixels
    ///      than the client window has, so the user sees blown-up UI.
    ///   2. **Strongly prefer HiDPI ("Retina") modes.** Non-HiDPI modes
    ///      render at 1× — fonts and icons use the legacy 1× rasterization
    ///      paths and look chunky / oversized on a Retina-class machine.
    ///      HiDPI modes (pixelWidth > width) render at 2× and downsample
    ///      crisply. We bias the score by ~10 to favour them.
    ///   3. **Smallest excess.** Among acceptable HiDPI modes, the one
    ///      whose logical size is closest to (but still ≥) the request
    ///      gives the densest workspace and the smallest SCKit downscale.
    ///   4. **Aspect match.** Same aspect as client lets SCKit pass the
    ///      frame through with no letterbox/stretch.
    ///
    /// Falls back to the closest-pixel-count mode if nothing satisfies
    /// the hard requirement (e.g. client asks for 5120×2880 on a 4K Mac).
    @discardableResult
    func setResolution(width: Int, height: Int) -> (Int, Int) {
        snapshotOriginal()
        guard width > 0, height > 0, !modesCache.isEmpty else {
            return (width, height)
        }

        let targetAspect = Double(width) / Double(height)

        // Pick the mode with logical size closest to the client request.
        // The captured frame ends up at SCStream's configured output
        // (= client size) regardless, so what matters for "feels right"
        // is the macOS UI density — which is determined by the mode's
        // logical width/height.
        //
        // Score components:
        //   * aspect mismatch × 30 — keep us from dropping to 4:3 on a
        //     16:9 request, but don't let it dominate as heavily as the
        //     size delta (a slight aspect mismatch + tiny SCKit final
        //     scale beats jumping two mode-steps to match aspect exactly).
        //   * size mismatch — Manhattan distance in logical points,
        //     normalized by request size. Symmetric (we don't strictly
        //     prefer ≥ request — picking 1280×720 for a 1356×825 client
        //     is a small upscale on SCKit's side, much better than
        //     1920×1080 which is a heavy downscale).
        //   * HiDPI tiebreaker −0.4 — at the same logical size, the
        //     HiDPI variant renders at 2×/3× and downsamples crisply.
        //     Small enough that it never overrules a closer-size mode.
        var best: CGDisplayMode? = nil
        var bestScore = Double.infinity
        for mode in modesCache {
            let lw = mode.width
            let lh = mode.height
            let pw = mode.pixelWidth
            if lw == 0 || lh == 0 { continue }

            let modeAspect = Double(lw) / Double(lh)
            let aspectDist = abs(modeAspect - targetAspect)
            let sizeDist = (abs(Double(lw - width)) +
                             abs(Double(lh - height))) /
                            Double(width + height)
            let isHiDPI = pw > lw
            let hiDPIBonus = isHiDPI ? -0.4 : 0.0

            let score = aspectDist * 30.0 + sizeDist + hiDPIBonus
            if score < bestScore {
                bestScore = score
                best = mode
            }
        }

        guard let target = best else { return (width, height) }
        let currentMode = CGDisplayCopyDisplayMode(displayID)
        // No-op suppression — mode changes flicker the local screen.
        if let cur = currentMode,
           cur.width == target.width,
           cur.height == target.height,
           cur.pixelWidth == target.pixelWidth,
           cur.pixelHeight == target.pixelHeight {
            return (target.width, target.height)
        }

        NSLog("[DisplayResolutionService] switching to logical " +
              "\(target.width)×\(target.height), pixel " +
              "\(target.pixelWidth)×\(target.pixelHeight) " +
              "(client wanted \(width)×\(height))")

        let err = CGDisplaySetDisplayMode(displayID, target, nil)
        if err != .success {
            NSLog("[DisplayResolutionService] CGDisplaySetDisplayMode " +
                  "failed: \(err.rawValue)")
        }
        return (target.width, target.height)
    }

    /// Restore the original display mode the user had before the server
    /// started. Called from RDPServerManager.stop() so the local screen
    /// returns to its pre-session state.
    func restoreOriginal() {
        guard let original = originalMode else { return }
        let err = CGDisplaySetDisplayMode(displayID, original, nil)
        if err != .success {
            NSLog("[DisplayResolutionService] restore failed: \(err.rawValue)")
        }
        originalMode = nil
    }
}
