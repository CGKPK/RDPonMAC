#include "RDPSubsystem.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <CoreGraphics/CoreGraphics.h>
#include <security/pam_appl.h>
#include <winpr/crt.h>
#include <winpr/synch.h>
#include <freerdp/codec/region.h>

// Global context shared with Swift
RDPMacSubsystemContext g_macSubsystemContext = {0};

// Log to the same file as Swift code
static void rdpmac_log(const char* fmt, ...) {
    const char* home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s", "/tmp/rdponmac.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fflush(f);
    fclose(f);
}

// Forward declarations for subsystem callbacks
static rdpShadowSubsystem* rdpmac_shadow_new(void);
static void rdpmac_shadow_free(rdpShadowSubsystem* subsystem);
static int rdpmac_shadow_init(rdpShadowSubsystem* subsystem);
static int rdpmac_shadow_uninit(rdpShadowSubsystem* subsystem);
static int rdpmac_shadow_start(rdpShadowSubsystem* subsystem);
static int rdpmac_shadow_stop(rdpShadowSubsystem* subsystem);
static UINT32 rdpmac_shadow_enum_monitors(MONITOR_DEF* monitors, UINT32 maxMonitors);

// Input callbacks
static BOOL rdpmac_shadow_keyboard_event(rdpShadowSubsystem* subsystem,
                                          rdpShadowClient* client,
                                          UINT16 flags, UINT8 code);
static BOOL rdpmac_shadow_unicode_keyboard_event(rdpShadowSubsystem* subsystem,
                                                  rdpShadowClient* client,
                                                  UINT16 flags, UINT16 code);
static BOOL rdpmac_shadow_mouse_event(rdpShadowSubsystem* subsystem,
                                       rdpShadowClient* client,
                                       UINT16 flags, UINT16 x, UINT16 y);

// Client callbacks
static BOOL rdpmac_shadow_client_connect(rdpShadowSubsystem* subsystem,
                                          rdpShadowClient* client);
static void rdpmac_shadow_client_disconnect(rdpShadowSubsystem* subsystem,
                                             rdpShadowClient* client);
static int rdpmac_shadow_authenticate(rdpShadowSubsystem* subsystem,
                                       rdpShadowClient* client,
                                       const char* user, const char* domain,
                                       const char* password);

// Protocol callbacks required for finalization sequence
static BOOL rdpmac_shadow_synchronize_event(rdpShadowSubsystem* subsystem,
                                             rdpShadowClient* client, UINT32 flags);
static BOOL rdpmac_shadow_client_capabilities(rdpShadowSubsystem* subsystem,
                                               rdpShadowClient* client);

// Entry point called by shadow_subsystem_set_entry
int RDPMacSubsystemEntry(RDP_SHADOW_ENTRY_POINTS* pEntryPoints)
{
    if (!pEntryPoints)
        return -1;

    pEntryPoints->New = rdpmac_shadow_new;
    pEntryPoints->Free = rdpmac_shadow_free;
    pEntryPoints->Init = rdpmac_shadow_init;
    pEntryPoints->Uninit = rdpmac_shadow_uninit;
    pEntryPoints->Start = rdpmac_shadow_start;
    pEntryPoints->Stop = rdpmac_shadow_stop;
    pEntryPoints->EnumMonitors = rdpmac_shadow_enum_monitors;

    return 0;
}

void rdpmac_register_subsystem_entry(void)
{
    shadow_subsystem_set_entry(RDPMacSubsystemEntry);
}

rdpShadowSubsystem* rdpmac_get_subsystem(rdpShadowServer* server)
{
    if (!server)
        return NULL;
    return server->subsystem;
}

// Subsystem lifecycle

static rdpShadowSubsystem* rdpmac_shadow_new(void)
{
    rdpShadowSubsystem* subsystem = (rdpShadowSubsystem*)calloc(1, sizeof(rdpShadowSubsystem));
    if (!subsystem)
        return NULL;
    return subsystem;
}

static void rdpmac_shadow_free(rdpShadowSubsystem* subsystem)
{
    if (subsystem)
        free(subsystem);
}

static int rdpmac_shadow_init(rdpShadowSubsystem* subsystem)
{
    if (!subsystem || !subsystem->server)
        return -1;

    // CRITICAL: Populate monitors array on the subsystem.
    // shadow_screen_new() reads subsystem->monitors[selectedMonitor] to create surfaces.
    subsystem->numMonitors = rdpmac_shadow_enum_monitors(subsystem->monitors, 16);
    rdpmac_log("[RDPonMAC] EnumMonitors: count=%u, primary=%dx%d\n",
            subsystem->numMonitors,
            subsystem->monitors[0].right - subsystem->monitors[0].left + 1,
            subsystem->monitors[0].bottom - subsystem->monitors[0].top + 1);
    if (subsystem->numMonitors == 0)
        return -1;

    // Set up input callbacks
    subsystem->KeyboardEvent = rdpmac_shadow_keyboard_event;
    subsystem->UnicodeKeyboardEvent = rdpmac_shadow_unicode_keyboard_event;
    subsystem->MouseEvent = rdpmac_shadow_mouse_event;

    // Set up client callbacks
    subsystem->ClientConnect = rdpmac_shadow_client_connect;
    subsystem->ClientDisconnect = rdpmac_shadow_client_disconnect;
    subsystem->Authenticate = rdpmac_shadow_authenticate;

    // ClientCapabilities is needed for proper capability exchange
    subsystem->ClientCapabilities = rdpmac_shadow_client_capabilities;
    // NOTE: Do NOT set SynchronizeEvent — it causes a sync loop that crashes the connection

    // Set default capture frame rate
    subsystem->captureFrameRate = 30;

    return 0;
}

static int rdpmac_shadow_uninit(rdpShadowSubsystem* subsystem)
{
    return 0;
}

static int rdpmac_shadow_start(rdpShadowSubsystem* subsystem)
{
    if (g_macSubsystemContext.onCaptureStart && g_macSubsystemContext.swiftContext)
        g_macSubsystemContext.onCaptureStart(g_macSubsystemContext.swiftContext);
    return 0;
}

static int rdpmac_shadow_stop(rdpShadowSubsystem* subsystem)
{
    if (g_macSubsystemContext.onCaptureStop && g_macSubsystemContext.swiftContext)
        g_macSubsystemContext.onCaptureStop(g_macSubsystemContext.swiftContext);
    return 0;
}

static UINT32 rdpmac_shadow_enum_monitors(MONITOR_DEF* monitors, UINT32 maxMonitors)
{
    if (maxMonitors < 1 || !monitors)
        return 0;

    // Get primary display LOGICAL resolution (not Retina physical pixels)
    // This must match what ScreenCaptureKit delivers
    CGDirectDisplayID mainDisplay = CGMainDisplayID();
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(mainDisplay);
    size_t width = mode ? CGDisplayModeGetWidth(mode) : 0;
    size_t height = mode ? CGDisplayModeGetHeight(mode) : 0;
    if (mode) CGDisplayModeRelease(mode);

    if (width == 0 || height == 0) {
        width = 1920;
        height = 1080;
    }

    // FreeRDP uses inclusive right/bottom: width = right - left + 1
    monitors[0].left = 0;
    monitors[0].top = 0;
    monitors[0].right = (INT32)(width - 1);
    monitors[0].bottom = (INT32)(height - 1);
    monitors[0].flags = 1; // MONITOR_PRIMARY

    return 1;
}

// Input event trampolines

static BOOL rdpmac_shadow_keyboard_event(rdpShadowSubsystem* subsystem,
                                          rdpShadowClient* client,
                                          UINT16 flags, UINT8 code)
{
    if (g_macSubsystemContext.onKeyboardEvent && g_macSubsystemContext.swiftContext)
        g_macSubsystemContext.onKeyboardEvent(g_macSubsystemContext.swiftContext, flags, code);
    return TRUE;
}

static BOOL rdpmac_shadow_unicode_keyboard_event(rdpShadowSubsystem* subsystem,
                                                  rdpShadowClient* client,
                                                  UINT16 flags, UINT16 code)
{
    if (g_macSubsystemContext.onUnicodeKeyboardEvent && g_macSubsystemContext.swiftContext)
        g_macSubsystemContext.onUnicodeKeyboardEvent(g_macSubsystemContext.swiftContext, flags, code);
    return TRUE;
}

static BOOL rdpmac_shadow_mouse_event(rdpShadowSubsystem* subsystem,
                                       rdpShadowClient* client,
                                       UINT16 flags, UINT16 x, UINT16 y)
{
    if (g_macSubsystemContext.onMouseEvent && g_macSubsystemContext.swiftContext)
        g_macSubsystemContext.onMouseEvent(g_macSubsystemContext.swiftContext, flags, x, y);
    return TRUE;
}

// Client connect/disconnect

static BOOL rdpmac_shadow_client_connect(rdpShadowSubsystem* subsystem,
                                          rdpShadowClient* client)
{
    rdpmac_log("[RDPonMAC] Client connected!\n");
    if (g_macSubsystemContext.onClientConnect && g_macSubsystemContext.swiftContext)
        g_macSubsystemContext.onClientConnect(g_macSubsystemContext.swiftContext);
    return TRUE;
}

static void rdpmac_shadow_client_disconnect(rdpShadowSubsystem* subsystem,
                                             rdpShadowClient* client)
{
    rdpmac_log("[RDPonMAC] Client disconnected\n");
    if (g_macSubsystemContext.onClientDisconnect && g_macSubsystemContext.swiftContext)
        g_macSubsystemContext.onClientDisconnect(g_macSubsystemContext.swiftContext);
}

// PAM conversation callback — supplies the password
static int rdpmac_pam_conv(int num_msg, const struct pam_message** msg,
                            struct pam_response** resp, void* appdata_ptr)
{
    const char* password = (const char*)appdata_ptr;
    struct pam_response* reply = calloc((size_t)num_msg, sizeof(struct pam_response));
    if (!reply) return PAM_CONV_ERR;

    for (int i = 0; i < num_msg; i++) {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            reply[i].resp = strdup(password ? password : "");
        }
    }
    *resp = reply;
    return PAM_SUCCESS;
}

static int rdpmac_shadow_authenticate(rdpShadowSubsystem* subsystem,
                                       rdpShadowClient* client,
                                       const char* user, const char* domain,
                                       const char* password)
{
    rdpmac_log("[RDPonMAC] Auth request: user=%s domain=%s\n",
            user ? user : "(null)", domain ? domain : "(null)");

    // If server has authentication disabled, allow all
    if (subsystem && subsystem->server && !subsystem->server->authentication)
        return 0;

    // Validate credentials against macOS user accounts via PAM
    if (!user || !password) {
        rdpmac_log("[RDPonMAC] Auth DENIED: missing user or password\n");
        return -1;
    }

    struct pam_conv conv = { rdpmac_pam_conv, (void*)password };
    pam_handle_t* pamh = NULL;
    int ret = pam_start("login", user, &conv, &pamh);
    if (ret != PAM_SUCCESS) {
        rdpmac_log("[RDPonMAC] Auth DENIED: pam_start failed (%d)\n", ret);
        return -1;
    }

    ret = pam_authenticate(pamh, 0);
    pam_end(pamh, ret);

    if (ret == PAM_SUCCESS) {
        rdpmac_log("[RDPonMAC] Auth OK: user=%s\n", user);
        return 0;
    } else {
        rdpmac_log("[RDPonMAC] Auth DENIED: user=%s (PAM error %d)\n", user, ret);
        return -1;
    }
}

// Protocol callbacks for RDP finalization sequence

static BOOL rdpmac_shadow_synchronize_event(rdpShadowSubsystem* subsystem,
                                             rdpShadowClient* client, UINT32 flags)
{
    rdpmac_log("[RDPonMAC] SynchronizeEvent: flags=0x%x\n", flags);
    return TRUE;
}

static BOOL rdpmac_shadow_client_capabilities(rdpShadowSubsystem* subsystem,
                                               rdpShadowClient* client)
{
    rdpmac_log("[RDPonMAC] ClientCapabilities received\n");
    return TRUE;
}

// Diagnostic: check if surface is ready for frame updates
bool rdpmac_check_surface(rdpShadowServer* server)
{
    if (!server) {
        rdpmac_log("[RDPonMAC] check_surface: server is NULL\n");
        return false;
    }
    if (!server->surface) {
        rdpmac_log("[RDPonMAC] check_surface: surface is NULL\n");
        return false;
    }
    rdpmac_log("[RDPonMAC] check_surface: OK (%ux%u format=0x%x data=%p)\n",
               server->surface->width, server->surface->height,
               server->surface->format, (void*)server->surface->data);
    if (!server->subsystem) {
        rdpmac_log("[RDPonMAC] check_surface: subsystem is NULL\n");
        return false;
    }
    rdpmac_log("[RDPonMAC] check_surface: subsystem OK, updateEvent=%p\n",
               (void*)server->subsystem->updateEvent);
    return true;
}

// Frame update function called by Swift

void rdpmac_update_frame(rdpShadowServer* server,
                         const uint8_t* data,
                         uint32_t width, uint32_t height, uint32_t stride,
                         uint32_t numRects, const RECTANGLE_16* rects)
{
    static int frameCount = 0;
    if (!server || !server->surface || !data) {
        if (frameCount == 0)
            rdpmac_log("[RDPonMAC] rdpmac_update_frame: dropped (server=%p surface=%p data=%p)\n",
                    (void*)server, server ? (void*)server->surface : NULL, (void*)data);
        return;
    }
    if (frameCount == 0)
        rdpmac_log("[RDPonMAC] First frame received: %ux%u stride=%u\n", width, height, stride);
    frameCount++;

    rdpShadowSurface* surface = server->surface;

    EnterCriticalSection(&surface->lock);

    // Resize surface if needed
    if (surface->width != width || surface->height != height) {
        if (surface->data)
            free(surface->data);
        surface->width = width;
        surface->height = height;
        surface->scanline = stride;
        surface->format = PIXEL_FORMAT_BGRX32;
        surface->data = (BYTE*)calloc(1, (size_t)height * stride);
        if (!surface->data) {
            LeaveCriticalSection(&surface->lock);
            return;
        }
    }

    // Copy frame data
    if (surface->scanline == stride) {
        memcpy(surface->data, data, (size_t)height * stride);
    } else {
        uint32_t copyStride = (stride < surface->scanline) ? stride : surface->scanline;
        for (uint32_t y = 0; y < height; y++) {
            memcpy(surface->data + y * surface->scanline,
                   data + y * stride,
                   copyStride);
        }
    }

    // Add dirty rectangles
    if (numRects > 0 && rects) {
        for (uint32_t i = 0; i < numRects; i++) {
            region16_union_rect(&surface->invalidRegion, &surface->invalidRegion, &rects[i]);
        }
    } else {
        // Full screen dirty
        RECTANGLE_16 fullRect;
        fullRect.left = 0;
        fullRect.top = 0;
        fullRect.right = (UINT16)width;
        fullRect.bottom = (UINT16)height;
        region16_union_rect(&surface->invalidRegion, &surface->invalidRegion, &fullRect);
    }

    LeaveCriticalSection(&surface->lock);
}

void rdpmac_notify_update(rdpShadowServer* server)
{
    static int notifyCount = 0;
    if (!server || !server->subsystem) {
        if (notifyCount == 0)
            rdpmac_log("[RDPonMAC] rdpmac_notify_update: NULL server=%p subsystem=%p\n",
                       (void*)server, server ? (void*)server->subsystem : NULL);
        return;
    }
    if (notifyCount == 0)
        rdpmac_log("[RDPonMAC] rdpmac_notify_update: first call, updateEvent=%p\n",
                   (void*)server->subsystem->updateEvent);
    notifyCount++;

    shadow_subsystem_frame_update(server->subsystem);
}
