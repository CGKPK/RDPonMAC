#include "RDPAudioBridge.h"
#include <stdlib.h>
#include <string.h>

static void rdp_audio_activated(RdpsndServerContext* context)
{
    RDPAudioContext* ctx = (RDPAudioContext*)context->data;
    if (!ctx)
        return;

    ctx->activated = true;

    // Find a compatible format - prefer 16-bit PCM at 44100Hz stereo
    for (size_t i = 0; i < context->num_client_formats; i++) {
        const AUDIO_FORMAT* clientFmt = &context->client_formats[i];
        if (clientFmt->wFormatTag == WAVE_FORMAT_PCM &&
            clientFmt->nChannels == 2 &&
            clientFmt->wBitsPerSample == 16) {
            ctx->selectedFormatIndex = (uint32_t)i;
            context->SelectFormat(context, (UINT16)i);

            ctx->serverFormat.wFormatTag = WAVE_FORMAT_PCM;
            ctx->serverFormat.nChannels = clientFmt->nChannels;
            ctx->serverFormat.nSamplesPerSec = clientFmt->nSamplesPerSec;
            ctx->serverFormat.wBitsPerSample = clientFmt->wBitsPerSample;
            ctx->serverFormat.nBlockAlign = clientFmt->nBlockAlign;
            ctx->serverFormat.nAvgBytesPerSec = clientFmt->nAvgBytesPerSec;
            return;
        }
    }

    // Fallback: accept first PCM format
    for (size_t i = 0; i < context->num_client_formats; i++) {
        const AUDIO_FORMAT* clientFmt = &context->client_formats[i];
        if (clientFmt->wFormatTag == WAVE_FORMAT_PCM) {
            ctx->selectedFormatIndex = (uint32_t)i;
            context->SelectFormat(context, (UINT16)i);

            ctx->serverFormat = *clientFmt;
            return;
        }
    }

    // No compatible format found
    ctx->activated = false;
}

RDPAudioContext* rdp_audio_init(HANDLE vcm)
{
    if (!vcm)
        return NULL;

    RDPAudioContext* ctx = (RDPAudioContext*)calloc(1, sizeof(RDPAudioContext));
    if (!ctx)
        return NULL;

    RdpsndServerContext* rdpsnd = rdpsnd_server_context_new(vcm);
    if (!rdpsnd) {
        free(ctx);
        return NULL;
    }

    rdpsnd->data = ctx;
    rdpsnd->Activated = rdp_audio_activated;

    // Advertise server formats: 16-bit PCM at various sample rates
    AUDIO_FORMAT serverFormats[3];
    memset(serverFormats, 0, sizeof(serverFormats));

    // 44100 Hz stereo
    serverFormats[0].wFormatTag = WAVE_FORMAT_PCM;
    serverFormats[0].nChannels = 2;
    serverFormats[0].nSamplesPerSec = 44100;
    serverFormats[0].wBitsPerSample = 16;
    serverFormats[0].nBlockAlign = 4;
    serverFormats[0].nAvgBytesPerSec = 176400;

    // 48000 Hz stereo
    serverFormats[1].wFormatTag = WAVE_FORMAT_PCM;
    serverFormats[1].nChannels = 2;
    serverFormats[1].nSamplesPerSec = 48000;
    serverFormats[1].wBitsPerSample = 16;
    serverFormats[1].nBlockAlign = 4;
    serverFormats[1].nAvgBytesPerSec = 192000;

    // 22050 Hz stereo
    serverFormats[2].wFormatTag = WAVE_FORMAT_PCM;
    serverFormats[2].nChannels = 2;
    serverFormats[2].nSamplesPerSec = 22050;
    serverFormats[2].wBitsPerSample = 16;
    serverFormats[2].nBlockAlign = 4;
    serverFormats[2].nAvgBytesPerSec = 88200;

    rdpsnd->server_formats = serverFormats;
    rdpsnd->num_server_formats = 3;

    ctx->rdpsnd = rdpsnd;

    if (rdpsnd->Initialize(rdpsnd, TRUE) != CHANNEL_RC_OK) {
        rdpsnd_server_context_free(rdpsnd);
        free(ctx);
        return NULL;
    }

    return ctx;
}

bool rdp_audio_send_samples(RDPAudioContext* ctx, const uint8_t* data, uint32_t numFrames)
{
    if (!ctx || !ctx->rdpsnd || !ctx->activated || !data || numFrames == 0)
        return false;

    return ctx->rdpsnd->SendSamples(ctx->rdpsnd, data, numFrames, 0) == CHANNEL_RC_OK;
}

bool rdp_audio_is_activated(RDPAudioContext* ctx)
{
    return ctx && ctx->activated;
}

void rdp_audio_free(RDPAudioContext* ctx)
{
    if (!ctx)
        return;

    if (ctx->rdpsnd) {
        rdpsnd_server_context_free(ctx->rdpsnd);
    }

    free(ctx);
}
