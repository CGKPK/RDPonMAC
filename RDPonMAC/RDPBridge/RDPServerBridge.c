// RDPServerBridge.c
// Thin Swift→C shim that forwards the public RDPServerHandle lifecycle calls
// onto the xRDP-based listener defined in RDPxRDPListener.[ch].
//
// The public Swift→C API surface declared in RDPonMAC-Bridging-Header.h and
// RDPServerBridge.h is intentionally unchanged — only the implementation
// underneath (formerly FreeRDP's shadow_server_*) has been replaced.

#include "RDPServerBridge.h"
#include "RDPServerInternal.h"
#include "RDPxRDPListener.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// ----------------------------------------------------------------------------
// Logging — kept identical to the previous FreeRDP-based implementation so
// existing log consumers continue to work.
// ----------------------------------------------------------------------------

static void bridge_log(const char* fmt, ...) {
    FILE* f = fopen("/tmp/rdponmac.log", "a");
    if (!f) { f = stderr; }
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    if (f != stderr) { fflush(f); fclose(f); }
}

// ----------------------------------------------------------------------------
// Internal server struct — opaque to Swift (handed back as RDPServerHandle).
// Holds the listener instance plus a stash of the configuration provided by
// rdp_server_configure(), which is applied to the listener inside
// rdp_server_init().
// ----------------------------------------------------------------------------

typedef struct {
    rdpmac_listener*       listener;
    rdpmac_listener_config cfg;
    bool                   configured;
    bool                   started;
} rdpmac_server;

// ============================================================================
// Public API
// ============================================================================

RDPServerHandle rdp_server_create(void)
{
    rdpmac_server* s = (rdpmac_server*)calloc(1, sizeof(*s));
    if (!s) {
        bridge_log("[RDPBridge] rdp_server_create: calloc failed\n");
        return NULL;
    }
    bridge_log("[RDPBridge] server created (%p)\n", (void*)s);
    return (RDPServerHandle)s;
}

bool rdp_server_configure(RDPServerHandle server,
                          uint32_t port,
                          const char* certFile,
                          const char* keyFile,
                          bool authentication)
{
    rdpmac_server* s = (rdpmac_server*)server;
    if (!s) return false;

    s->cfg.port           = port;
    s->cfg.authentication = authentication;

    free(s->cfg.certFile);
    s->cfg.certFile = certFile ? strdup(certFile) : NULL;

    free(s->cfg.keyFile);
    s->cfg.keyFile  = keyFile  ? strdup(keyFile)  : NULL;

    s->configured = true;

    bridge_log("[RDPBridge] configured: port=%u auth=%d cert=%s key=%s\n",
               port, (int)authentication,
               certFile ? certFile : "(none)",
               keyFile  ? keyFile  : "(none)");
    return true;
}

int rdp_server_init(RDPServerHandle server)
{
    rdpmac_server* s = (rdpmac_server*)server;
    if (!s || !s->configured) {
        bridge_log("[RDPBridge] rdp_server_init: not configured\n");
        return -1;
    }

    s->listener = rdpmac_listener_create();
    if (!s->listener) {
        bridge_log("[RDPBridge] rdp_server_init: listener_create failed\n");
        return -1;
    }

    if (!rdpmac_listener_configure(s->listener, &s->cfg)) {
        bridge_log("[RDPBridge] rdp_server_init: listener_configure failed\n");
        rdpmac_listener_free(s->listener);
        s->listener = NULL;
        return -1;
    }

    bridge_log("[RDPBridge] init OK\n");
    return 0;
}

int rdp_server_start(RDPServerHandle server)
{
    rdpmac_server* s = (rdpmac_server*)server;
    if (!s || !s->listener) {
        bridge_log("[RDPBridge] rdp_server_start: not initialised\n");
        return -1;
    }

    int rv = rdpmac_listener_start(s->listener);
    if (rv == 0) {
        s->started = true;
        bridge_log("[RDPBridge] start OK\n");
    } else {
        bridge_log("[RDPBridge] start failed: %d\n", rv);
    }
    return rv;
}

int rdp_server_stop(RDPServerHandle server)
{
    rdpmac_server* s = (rdpmac_server*)server;
    if (!s || !s->listener) return -1;

    int rv = rdpmac_listener_stop(s->listener);
    s->started = false;
    bridge_log("[RDPBridge] stop returned %d\n", rv);
    return rv;
}

int rdp_server_uninit(RDPServerHandle server)
{
    rdpmac_server* s = (rdpmac_server*)server;
    if (!s) return -1;
    if (!s->listener) return 0;

    rdpmac_listener_free(s->listener);
    s->listener = NULL;
    bridge_log("[RDPBridge] uninit OK\n");
    return 0;
}

void rdp_server_free(RDPServerHandle server)
{
    rdpmac_server* s = (rdpmac_server*)server;
    if (!s) return;

    if (s->listener) {
        rdpmac_listener_free(s->listener);
        s->listener = NULL;
    }
    free(s->cfg.certFile);
    free(s->cfg.keyFile);

    bridge_log("[RDPBridge] server freed (%p)\n", (void*)s);
    free(s);
}

// ----------------------------------------------------------------------------
// Internal accessor for other bridge TUs (not exposed in RDPServerBridge.h).
// ----------------------------------------------------------------------------

rdpmac_listener* rdp_server_get_listener_internal(RDPServerHandle s)
{
    if (!s) return NULL;
    return ((rdpmac_server*)s)->listener;
}
