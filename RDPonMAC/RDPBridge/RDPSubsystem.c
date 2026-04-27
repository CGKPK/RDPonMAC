// RDPSubsystem.c
// macOS-specific glue for the xRDP-based RDPonMAC bridge.
//
// What lives here:
//   - the Swift callback table (g_macSubsystemContext)
//   - shared logging (rdpmac_log) used by every C bridge module
//   - PAM-backed authentication (rdpmac_authenticate) called from the listener
//   - primary-display query (rdpmac_get_primary_display_size)
//   - frame-delivery shim (rdpmac_update_frame / rdpmac_notify_update) which
//     forwards into the listener's per-client send path
//   - rdpmac_check_surface stub (kept for Swift API stability)
//
// The FreeRDP shadow-subsystem entry points and surface bookkeeping that
// used to live in this file have moved to RDPxRDPListener.[ch].

#include "RDPSubsystem.h"
#include "RDPServerInternal.h"   // brings in RDPServerHandle + listener API
#include "RDPxRDPListener.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CoreGraphics/CoreGraphics.h>
#include <security/pam_appl.h>

// ----------------------------------------------------------------------------
// Global Swift callback table
// ----------------------------------------------------------------------------

RDPMacSubsystemContext g_macSubsystemContext = {0};

// ----------------------------------------------------------------------------
// Logging
// ----------------------------------------------------------------------------

void rdpmac_log(const char* fmt, ...)
{
    FILE* f = fopen("/tmp/rdponmac.log", "a");
    if (!f) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fflush(f);
    fclose(f);
}

// ----------------------------------------------------------------------------
// PAM authentication
// ----------------------------------------------------------------------------

// PAM conversation callback — feeds the user-supplied password back to PAM.
static int rdpmac_pam_conv(int num_msg, const struct pam_message** msg,
                            struct pam_response** resp, void* appdata_ptr)
{
    const char* password = (const char*)appdata_ptr;
    struct pam_response* reply = calloc((size_t)num_msg, sizeof(struct pam_response));
    if (!reply) {
        return PAM_CONV_ERR;
    }

    for (int i = 0; i < num_msg; i++) {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
            msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            reply[i].resp = strdup(password ? password : "");
        }
    }
    *resp = reply;
    return PAM_SUCCESS;
}

int rdpmac_authenticate(const char* user, const char* domain, const char* password)
{
    rdpmac_log("[RDPonMAC] Auth request: user=%s domain=%s\n",
               user ? user : "(null)", domain ? domain : "(null)");

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
    }

    rdpmac_log("[RDPonMAC] Auth DENIED: user=%s (PAM error %d)\n", user, ret);
    return -1;
}

// ----------------------------------------------------------------------------
// Primary-display query
// ----------------------------------------------------------------------------

void rdpmac_get_primary_display_size(uint32_t* outWidth, uint32_t* outHeight)
{
    // Report the primary display's native pixel resolution. This matches
    // what ScreenCaptureService.swift configures the SCStream with, so the
    // dimensions libxrdp negotiates with the client line up exactly with
    // the buffer we feed into rdpmac_listener_push_frame.
    CGDirectDisplayID mainDisplay = CGMainDisplayID();
    size_t w = CGDisplayPixelsWide(mainDisplay);
    size_t h = CGDisplayPixelsHigh(mainDisplay);
    if (w == 0 || h == 0) { w = 1920; h = 1080; }
    if (outWidth)  *outWidth  = (uint32_t)w;
    if (outHeight) *outHeight = (uint32_t)h;
}

// ----------------------------------------------------------------------------
// Frame delivery — forwards from Swift's capture pipeline into the listener.
// ----------------------------------------------------------------------------

// The Swift-visible prototype in RDPonMAC-Bridging-Header.h declares the
// final argument as `const RDPRect* rects`. RDPRect and rdpmac_rect have
// identical 4 x uint16_t layouts, so the calling convention is ABI-equivalent;
// we use rdpmac_rect here to avoid pulling the Swift bridging header into
// this pure-C translation unit.
void rdpmac_update_frame(RDPServerHandle server,
                         const uint8_t* data,
                         uint32_t width, uint32_t height, uint32_t stride,
                         uint32_t numRects, const rdpmac_rect* rects)
{
    if (!server || !data) {
        return;
    }

    rdpmac_listener* listener = rdp_server_get_listener_internal(server);
    if (!listener) {
        return;
    }

    rdpmac_listener_push_frame(listener,
                               data,
                               width, height, stride,
                               numRects,
                               rects);
}

// Forwarder for cursor shape updates. Called from CursorService.swift when
// NSCursor.current changes.
void rdpmac_update_pointer(RDPServerHandle server,
                           const uint8_t* rgba,
                           uint32_t width, uint32_t height,
                           uint32_t hotX, uint32_t hotY)
{
    if (!server || !rgba || width == 0 || height == 0) {
        return;
    }
    rdpmac_listener* listener = rdp_server_get_listener_internal(server);
    if (!listener) {
        return;
    }
    rdpmac_listener_push_pointer(listener, rgba, width, height, hotX, hotY);
}

void rdpmac_notify_update(RDPServerHandle server)
{
    // Under libxrdp, frame delivery in rdpmac_update_frame is synchronous —
    // there is no separate encoder thread to wake. The Swift call site is
    // kept for API compatibility but is now a no-op.
    (void)server;
}

// ----------------------------------------------------------------------------
// Surface diagnostic — reduced to a "is the server handle valid" check now
// that the FreeRDP shadow surface no longer exists. Swift code still calls
// this, so the symbol is retained.
// ----------------------------------------------------------------------------

bool rdpmac_check_surface(RDPServerHandle server)
{
    return server != NULL;
}
