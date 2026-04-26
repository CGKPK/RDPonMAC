#include "RDPClipboardBridge.h"
#include <stdlib.h>
#include <string.h>

// Cliprdr callbacks from the client

static UINT rdp_cliprdr_client_format_list(CliprdrServerContext* context,
                                            const CLIPRDR_FORMAT_LIST* formatList)
{
    RDPClipboardContext* ctx = (RDPClipboardContext*)context->custom;
    if (!ctx || !ctx->onFormatList)
        return CHANNEL_RC_OK;

    uint32_t numFormats = formatList->numFormats;
    uint32_t* formatIds = (uint32_t*)calloc(numFormats, sizeof(uint32_t));
    if (!formatIds)
        return ERROR_OUTOFMEMORY;

    for (uint32_t i = 0; i < numFormats; i++) {
        formatIds[i] = formatList->formats[i].formatId;
    }

    ctx->onFormatList(ctx->swiftContext, numFormats, formatIds);
    free(formatIds);

    // Send format list response acknowledging receipt
    CLIPRDR_FORMAT_LIST_RESPONSE response = {0};
    response.common.msgFlags = CB_RESPONSE_OK;
    return context->ServerFormatListResponse(context, &response);
}

static UINT rdp_cliprdr_client_format_data_request(CliprdrServerContext* context,
                                                     const CLIPRDR_FORMAT_DATA_REQUEST* request)
{
    RDPClipboardContext* ctx = (RDPClipboardContext*)context->custom;
    if (!ctx || !ctx->onDataRequest)
        return CHANNEL_RC_OK;

    ctx->onDataRequest(ctx->swiftContext, request->requestedFormatId);
    return CHANNEL_RC_OK;
}

static UINT rdp_cliprdr_client_format_data_response(CliprdrServerContext* context,
                                                      const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
    RDPClipboardContext* ctx = (RDPClipboardContext*)context->custom;
    if (!ctx || !ctx->onDataResponse)
        return CHANNEL_RC_OK;

    ctx->onDataResponse(ctx->swiftContext,
                        response->requestedFormatData,
                        response->common.dataLen,
                        0); // formatId not in response, tracked by caller
    return CHANNEL_RC_OK;
}

RDPClipboardContext* rdp_clipboard_init(HANDLE vcm, void* swiftCtx)
{
    if (!vcm)
        return NULL;

    RDPClipboardContext* ctx = (RDPClipboardContext*)calloc(1, sizeof(RDPClipboardContext));
    if (!ctx)
        return NULL;

    ctx->swiftContext = swiftCtx;

    CliprdrServerContext* cliprdr = cliprdr_server_context_new(vcm);
    if (!cliprdr) {
        free(ctx);
        return NULL;
    }

    cliprdr->custom = ctx;
    cliprdr->useLongFormatNames = TRUE;
    cliprdr->streamFileClipEnabled = FALSE;
    cliprdr->fileClipNoFilePaths = TRUE;

    // Set client callbacks
    cliprdr->ClientFormatList = rdp_cliprdr_client_format_list;
    cliprdr->ClientFormatDataRequest = rdp_cliprdr_client_format_data_request;
    cliprdr->ClientFormatDataResponse = rdp_cliprdr_client_format_data_response;

    ctx->cliprdr = cliprdr;

    // Open and start the channel
    if (cliprdr->Open(cliprdr) != CHANNEL_RC_OK) {
        cliprdr_server_context_free(cliprdr);
        free(ctx);
        return NULL;
    }

    if (cliprdr->Start(cliprdr) != CHANNEL_RC_OK) {
        cliprdr->Close(cliprdr);
        cliprdr_server_context_free(cliprdr);
        free(ctx);
        return NULL;
    }

    return ctx;
}

bool rdp_clipboard_send_format_list(RDPClipboardContext* ctx, uint32_t numFormats,
                                     const uint32_t* formatIds, const char** formatNames)
{
    if (!ctx || !ctx->cliprdr || numFormats == 0)
        return false;

    CLIPRDR_FORMAT* formats = (CLIPRDR_FORMAT*)calloc(numFormats, sizeof(CLIPRDR_FORMAT));
    if (!formats)
        return false;

    for (uint32_t i = 0; i < numFormats; i++) {
        formats[i].formatId = formatIds[i];
        if (formatNames && formatNames[i])
            formats[i].formatName = _strdup(formatNames[i]);
    }

    CLIPRDR_FORMAT_LIST formatList = {0};
    formatList.numFormats = numFormats;
    formatList.formats = formats;
    formatList.common.msgType = CB_FORMAT_LIST;

    UINT result = ctx->cliprdr->ServerFormatList(ctx->cliprdr, &formatList);

    for (uint32_t i = 0; i < numFormats; i++) {
        free(formats[i].formatName);
    }
    free(formats);

    return result == CHANNEL_RC_OK;
}

bool rdp_clipboard_send_data_response(RDPClipboardContext* ctx, const uint8_t* data, uint32_t length)
{
    if (!ctx || !ctx->cliprdr)
        return false;

    CLIPRDR_FORMAT_DATA_RESPONSE response = {0};
    response.common.msgFlags = CB_RESPONSE_OK;
    response.common.dataLen = length;
    response.requestedFormatData = data;

    return ctx->cliprdr->ServerFormatDataResponse(ctx->cliprdr, &response) == CHANNEL_RC_OK;
}

bool rdp_clipboard_request_data(RDPClipboardContext* ctx, uint32_t formatId)
{
    if (!ctx || !ctx->cliprdr)
        return false;

    CLIPRDR_FORMAT_DATA_REQUEST request = {0};
    request.requestedFormatId = formatId;

    return ctx->cliprdr->ServerFormatDataRequest(ctx->cliprdr, &request) == CHANNEL_RC_OK;
}

void rdp_clipboard_free(RDPClipboardContext* ctx)
{
    if (!ctx)
        return;

    if (ctx->cliprdr) {
        ctx->cliprdr->Stop(ctx->cliprdr);
        ctx->cliprdr->Close(ctx->cliprdr);
        cliprdr_server_context_free(ctx->cliprdr);
    }

    free(ctx);
}
