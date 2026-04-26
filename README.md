# RDPonMAC

A native RDP server for macOS — connect to your Mac from any standard RDP
client (mstsc.exe, sdl-freerdp, etc.).

Built on **xRDP's libxrdp** for the protocol layer (Apache 2.0 licensed) and
ScreenCaptureKit + CGEvent / IOKit HID for the macOS platform layer.

## Features

- ✅ TLS-encrypted RDP connections (port 3389)
- ✅ Mac user authentication via PAM
- ✅ Screen capture via ScreenCaptureKit (with CGDisplayCreateImage fallback for the login screen)
- ✅ Keyboard + mouse input injection via CGEvent (with IOKit HID fallback)
- ✅ Coordinate scaling between RDP capture resolution and Mac display

## Requirements

- macOS 14+ (Sonoma or newer)
- Xcode 15+
- Homebrew packages: `cmake`, `libtool`, `pkgconf`, `openssl@3`

```bash
brew install cmake libtool pkgconf openssl@3
```

## Building

The Xcode project has a Run Script Build Phase that automatically:
1. Clones `xrdp` v0.10 into `Vendor/xrdp/` (first build only)
2. Applies our local patches from `Vendor/xrdp_patches/`
3. Builds `libxrdp.a`, `libcommon.a`, and `librfxencode.a`
4. Stages headers under `Vendor/xrdp/build/include/`

Just open `RDPonMAC.xcodeproj` in Xcode and build. The first build takes ~2
minutes (xrdp clone + autotools bootstrap + compile); subsequent builds are
incremental.

## Architecture

```
[RDP Client] ⟶ TLS ⟶ [RDPxRDPListener.c (kqueue/pthread per client)]
                              ⟶ libxrdp (protocol)
                              ⟶ g_macSubsystemContext callbacks
                              ⟶ Swift Services
                                  - ScreenCaptureService (frame capture)
                                  - InputInjectionService (mouse/keyboard)
                                  - RDPServerManager (lifecycle)
```

Key C bridge files:
- `RDPonMAC/RDPBridge/RDPxRDPListener.c` — TCP listener + per-client libxrdp session
- `RDPonMAC/RDPBridge/RDPServerBridge.c` — Swift→C shim
- `RDPonMAC/RDPBridge/RDPSubsystem.c` — PAM auth + global callback table + frame delivery

## Known limitations

- **Capture resolution capped at 1920×1080** — slow-path bitmap updates
  produce too many PDUs at full Retina, causing strict clients (mstsc) to
  disconnect. Phase 8 introduces RemoteFX to lift this.
- **Frame rate throttled to ~5 FPS** for the same reason.
- **Login screen input requires the user to be logged in first** — IOKit HID
  injection at the login window has restrictive entitlement requirements.
- Clipboard sync, audio redirection, multi-monitor, and file transfer are
  planned for Phase 8.

## License

This project is Apache 2.0. xRDP is also Apache 2.0.
