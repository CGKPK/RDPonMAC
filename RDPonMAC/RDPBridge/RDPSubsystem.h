#ifndef RDPSubsystem_h
#define RDPSubsystem_h

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/server/shadow.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Callback typedefs
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

int RDPMacSubsystemEntry(RDP_SHADOW_ENTRY_POINTS* pEntryPoints);
void rdpmac_register_subsystem_entry(void);

// These use RECTANGLE_16 internally, exposed as RDPRect via bridging header
void rdpmac_update_frame(rdpShadowServer* server,
                         const uint8_t* data,
                         uint32_t width, uint32_t height, uint32_t stride,
                         uint32_t numRects, const RECTANGLE_16* rects);
void rdpmac_notify_update(rdpShadowServer* server);

#ifdef __cplusplus
}
#endif

#endif
