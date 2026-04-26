#ifndef RDPAudioBridge_h
#define RDPAudioBridge_h

#include <freerdp/freerdp.h>
#include <freerdp/server/rdpsnd.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Audio bridge context
typedef struct {
    RdpsndServerContext* rdpsnd;
    AUDIO_FORMAT serverFormat;
    bool activated;
    uint32_t selectedFormatIndex;
} RDPAudioContext;

// Initialize audio channel for a client
RDPAudioContext* rdp_audio_init(HANDLE vcm);

// Send audio samples to client
bool rdp_audio_send_samples(RDPAudioContext* ctx,
                            const uint8_t* data,
                            uint32_t numFrames);

// Check if audio is activated (client negotiation complete)
bool rdp_audio_is_activated(RDPAudioContext* ctx);

// Cleanup
void rdp_audio_free(RDPAudioContext* ctx);

#ifdef __cplusplus
}
#endif

#endif
