// RDPClipboardBridge.h
// Stubbed in the xRDP migration — original FreeRDP-based implementation will
// be re-written on top of libxrdp's CLIPRDR static virtual channel API in
// Phase 8a. For now this header keeps the Swift surface stable but defines
// no FreeRDP-dependent symbols.

#ifndef RDPClipboardBridge_h
#define RDPClipboardBridge_h

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Clipboard format IDs
#define RDP_CF_TEXT          1
#define RDP_CF_UNICODETEXT   13
#define RDP_CF_DIB           8

typedef struct RDPClipboardContext RDPClipboardContext;

typedef void (*RDPClipboardFormatListCallback)(void* ctx, uint32_t numFormats, const uint32_t* formatIds);
typedef void (*RDPClipboardDataRequestCallback)(void* ctx, uint32_t formatId);
typedef void (*RDPClipboardDataResponseCallback)(void* ctx, const uint8_t* data, uint32_t length, uint32_t formatId);

#ifdef __cplusplus
}
#endif

#endif
