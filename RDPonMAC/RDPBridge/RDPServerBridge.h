#ifndef RDPServerBridge_h
#define RDPServerBridge_h

#include <freerdp/freerdp.h>
#include <freerdp/server/shadow.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// These functions use rdpShadowServer* internally but are exposed
// to Swift as void* (RDPServerHandle) via the bridging header.

rdpShadowServer* rdp_server_create(void);

bool rdp_server_configure(rdpShadowServer* server,
                          uint32_t port,
                          const char* certFile,
                          const char* keyFile,
                          bool authentication);

int rdp_server_init(rdpShadowServer* server);
int rdp_server_start(rdpShadowServer* server);
int rdp_server_stop(rdpShadowServer* server);
int rdp_server_uninit(rdpShadowServer* server);
void rdp_server_free(rdpShadowServer* server);

#ifdef __cplusplus
}
#endif

#endif
