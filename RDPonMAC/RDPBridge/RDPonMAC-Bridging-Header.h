// RDPonMAC-Bridging-Header.h
// This header exposes ONLY our C bridge API to Swift.
// FreeRDP types are hidden behind opaque pointers to avoid
// namespace collisions (WinPR GUID vs Foundation UUID).

#ifndef RDPonMAC_Bridging_Header_h
#define RDPonMAC_Bridging_Header_h

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Opaque pointer types (hide FreeRDP internals from Swift)
// ============================================================
typedef void* RDPServerHandle;
typedef void* RDPSurfaceHandle;
typedef void* RDPSettingsHandle;
typedef void* RDPClipboardHandle;
typedef void* RDPAudioHandle;

// ============================================================
// Subsystem callbacks (set by Swift before server init)
// ============================================================
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

// ============================================================
// Server lifecycle
// ============================================================
RDPServerHandle rdp_server_create(void);
bool rdp_server_configure(RDPServerHandle server,
                          uint32_t port,
                          const char* certFile,
                          const char* keyFile,
                          bool authentication);
int rdp_server_init(RDPServerHandle server);
int rdp_server_start(RDPServerHandle server);
int rdp_server_stop(RDPServerHandle server);
int rdp_server_uninit(RDPServerHandle server);
void rdp_server_free(RDPServerHandle server);

// ============================================================
// Frame updates
// ============================================================
typedef struct {
    uint16_t left;
    uint16_t top;
    uint16_t right;
    uint16_t bottom;
} RDPRect;

void rdpmac_update_frame(RDPServerHandle server,
                         const uint8_t* data,
                         uint32_t width, uint32_t height, uint32_t stride,
                         uint32_t numRects, const RDPRect* rects);
void rdpmac_notify_update(RDPServerHandle server);
bool rdpmac_check_surface(RDPServerHandle server);  // returns true if surface is ready

// ============================================================
// Input bridge
// ============================================================
#define RDP_KBD_FLAGS_EXTENDED  0x0100
#define RDP_KBD_FLAGS_DOWN      0x4000
#define RDP_KBD_FLAGS_RELEASE   0x8000

#define RDP_PTR_FLAGS_WHEEL           0x0200
#define RDP_PTR_FLAGS_WHEEL_NEGATIVE  0x0100
#define RDP_PTR_FLAGS_MOVE            0x0800
#define RDP_PTR_FLAGS_DOWN            0x8000
#define RDP_PTR_FLAGS_BUTTON1         0x1000
#define RDP_PTR_FLAGS_BUTTON2         0x2000
#define RDP_PTR_FLAGS_BUTTON3         0x4000

int16_t rdp_scancode_to_mac_keycode(uint8_t scancode, uint16_t flags);

#ifdef __cplusplus
}
#endif

#endif
