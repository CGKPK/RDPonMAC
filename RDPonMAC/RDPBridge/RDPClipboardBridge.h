#ifndef RDPClipboardBridge_h
#define RDPClipboardBridge_h

#include <freerdp/freerdp.h>
#include <freerdp/server/cliprdr.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Clipboard format IDs
#define RDP_CF_TEXT          1
#define RDP_CF_UNICODETEXT   13
#define RDP_CF_DIB           8

// Callback typedefs for Swift
typedef void (*RDPClipboardFormatListCallback)(void* ctx, uint32_t numFormats, const uint32_t* formatIds);
typedef void (*RDPClipboardDataRequestCallback)(void* ctx, uint32_t formatId);
typedef void (*RDPClipboardDataResponseCallback)(void* ctx, const uint8_t* data, uint32_t length, uint32_t formatId);

// Clipboard context for bridging
typedef struct {
    void* swiftContext;
    RDPClipboardFormatListCallback onFormatList;
    RDPClipboardDataRequestCallback onDataRequest;
    RDPClipboardDataResponseCallback onDataResponse;
    CliprdrServerContext* cliprdr;
} RDPClipboardContext;

// Initialize clipboard channel for a client
RDPClipboardContext* rdp_clipboard_init(HANDLE vcm, void* swiftCtx);

// Send format list to client (server has new clipboard data)
bool rdp_clipboard_send_format_list(RDPClipboardContext* ctx, uint32_t numFormats, const uint32_t* formatIds, const char** formatNames);

// Send data response to client
bool rdp_clipboard_send_data_response(RDPClipboardContext* ctx, const uint8_t* data, uint32_t length);

// Request data from client
bool rdp_clipboard_request_data(RDPClipboardContext* ctx, uint32_t formatId);

// Cleanup
void rdp_clipboard_free(RDPClipboardContext* ctx);

#ifdef __cplusplus
}
#endif

#endif
