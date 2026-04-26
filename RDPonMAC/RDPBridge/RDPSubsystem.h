// RDPSubsystem.h
// macOS-specific glue for the xRDP-based RDPonMAC bridge:
//   - the Swift callback table (g_macSubsystemContext)
//   - logging used by both this file and RDPxRDPListener.c
//   - PAM-based authentication helper invoked from the listener
//   - primary-display query used to size the libxrdp framebuffer
//
// FreeRDP shadow-subsystem code that previously lived here has been removed;
// the new connection/event plumbing lives in RDPxRDPListener.[ch].

#ifndef RDPSubsystem_h
#define RDPSubsystem_h

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------------
// Swift callback table
// ----------------------------------------------------------------------------
//
// The same struct is also published in RDPonMAC-Bridging-Header.h so Swift
// can populate it; it is repeated here so pure-C translation units can
// include this header without dragging in the bridging header (and through
// it the Foundation/UIKit umbrella).

typedef void (*RDPCaptureStartCallback)(void* ctx);
typedef void (*RDPCaptureStopCallback)(void* ctx);
typedef void (*RDPKeyboardEventCallback)(void* ctx, uint16_t flags, uint8_t code);
typedef void (*RDPUnicodeKeyboardEventCallback)(void* ctx, uint16_t flags, uint16_t code);
typedef void (*RDPMouseEventCallback)(void* ctx, uint16_t flags, uint16_t x, uint16_t y);
typedef void (*RDPClientConnectCallback)(void* ctx);
typedef void (*RDPClientDisconnectCallback)(void* ctx);

typedef struct {
    void* swiftContext;
    RDPCaptureStartCallback onCaptureStart;
    RDPCaptureStopCallback onCaptureStop;
    RDPKeyboardEventCallback onKeyboardEvent;
    RDPUnicodeKeyboardEventCallback onUnicodeKeyboardEvent;
    RDPMouseEventCallback onMouseEvent;
    RDPClientConnectCallback onClientConnect;
    RDPClientDisconnectCallback onClientDisconnect;
} RDPMacSubsystemContext;

extern RDPMacSubsystemContext g_macSubsystemContext;

// ----------------------------------------------------------------------------
// Logging — single file used by every C module in the bridge.
// Non-static so RDPxRDPListener.c (which forward-declares it as extern) can
// share the implementation.
// ----------------------------------------------------------------------------
void rdpmac_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// ----------------------------------------------------------------------------
// Authentication — PAM-backed credential check for incoming RDP clients.
// Called from RDPxRDPListener.c only when listener config has authentication
// enabled; returns 0 on success, -1 on failure.
// ----------------------------------------------------------------------------
int rdpmac_authenticate(const char* user, const char* domain, const char* password);

// ----------------------------------------------------------------------------
// Primary-display query — used by the listener to advertise a screen size to
// libxrdp during capability negotiation. Falls back to 1920x1080 if the
// CoreGraphics query fails for any reason.
// ----------------------------------------------------------------------------
void rdpmac_get_primary_display_size(uint32_t* width, uint32_t* height);

#ifdef __cplusplus
}
#endif

#endif
