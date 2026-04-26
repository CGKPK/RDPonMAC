// RDPServerInternal.h
// Internal-only API exposed by RDPServerBridge.c to other C translation units
// inside the bridge (specifically RDPSubsystem.c, which needs to reach the
// listener attached to a server handle when forwarding frames).
//
// This header is NOT part of the Swift-visible bridging surface.

#ifndef RDPServerInternal_h
#define RDPServerInternal_h

#include "RDPServerBridge.h"
#include "RDPxRDPListener.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the rdpmac_listener owned by the server handle, or NULL if the
// handle is NULL or has not been initialised yet.
rdpmac_listener* rdp_server_get_listener_internal(RDPServerHandle s);

#ifdef __cplusplus
}
#endif

#endif
