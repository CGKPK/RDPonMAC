#ifndef RDPServerBridge_h
#define RDPServerBridge_h

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle for the server instance — same definition as in the Swift
// bridging header, repeated here so this header is self-contained for C
// translation units that don't include the bridging header directly.
#ifndef RDPServerHandle_DEFINED
#define RDPServerHandle_DEFINED
typedef void* RDPServerHandle;
#endif

// Server lifecycle. The implementation lives in RDPServerBridge.c and forwards
// onto the xRDP-based listener in RDPxRDPListener.[ch].
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

#ifdef __cplusplus
}
#endif

#endif
