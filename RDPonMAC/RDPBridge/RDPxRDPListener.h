// RDPxRDPListener.h
// Internal-to-the-bridge module that owns a TCP listener on the RDP port,
// accepts client connections, and runs one libxrdp session per client.
//
// This module replaces FreeRDP's shadow_server_* family of APIs.
// The Swift→C public surface (rdp_server_create/init/start/stop/free,
// rdpmac_update_frame, rdpmac_notify_update, g_macSubsystemContext) is
// unchanged — RDPServerBridge.c forwards into this module.

#ifndef RDPxRDPListener_h
#define RDPxRDPListener_h

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque types
typedef struct rdpmac_listener rdpmac_listener;
typedef struct rdpmac_client rdpmac_client;

// Configuration passed when starting the listener.
typedef struct {
    uint32_t port;          // TCP port to listen on (default 3389)
    bool authentication;    // true → require valid PAM credentials
    char* certFile;         // PEM cert path (owned by caller; we copy)
    char* keyFile;          // PEM key path (owned by caller; we copy)
} rdpmac_listener_config;

// Lifecycle — called from RDPServerBridge.c
rdpmac_listener* rdpmac_listener_create(void);
bool rdpmac_listener_configure(rdpmac_listener* l, const rdpmac_listener_config* cfg);
int rdpmac_listener_start(rdpmac_listener* l);  // 0 on success
int rdpmac_listener_stop(rdpmac_listener* l);
void rdpmac_listener_free(rdpmac_listener* l);

// Frame delivery — called from rdpmac_update_frame() in RDPSubsystem.c.
// Iterates all connected clients and pushes the frame data via libxrdp.
// `data` is BGRX32 pixel data, `stride` is bytes per row.
// `numRects` is dirty rect count (0 = full screen update).
typedef struct {
    uint16_t left;
    uint16_t top;
    uint16_t right;
    uint16_t bottom;
} rdpmac_rect;

void rdpmac_listener_push_frame(rdpmac_listener* l,
                                 const uint8_t* data,
                                 uint32_t width, uint32_t height, uint32_t stride,
                                 uint32_t numRects, const rdpmac_rect* rects);

// Diagnostics
int rdpmac_listener_client_count(rdpmac_listener* l);

#ifdef __cplusplus
}
#endif

#endif
