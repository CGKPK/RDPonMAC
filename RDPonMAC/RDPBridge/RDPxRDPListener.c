// RDPxRDPListener.c
// Internal-to-the-bridge module that owns a TCP listener on the RDP port,
// accepts client connections, and runs one libxrdp session per client.
//
// This is the macOS-side equivalent of xrdp's daemon top half (xrdp_listen.c
// + xrdp_process.c). It uses xrdp's `trans` layer for socket I/O and
// libxrdp for the RDP protocol state machine. Threading is plain pthreads:
//
//   listener thread  : 1, owns trans_listen socket, accepts new connections.
//   client threads   : N, one per connected RDP client, runs the libxrdp
//                      main loop pattern from xrdp_process_main_loop().
//
// Cross-thread access to the client list is mutex-protected. Frame delivery
// (rdpmac_listener_push_frame) is called from the capture thread and walks
// the same client list under that mutex.

#include "RDPxRDPListener.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>

// xRDP headers (search paths configured by the Xcode project in Phase 6).
// config_ac.h must come first — log.h/parse.h check that CONFIG_AC_H is
// defined and #error if not.
#include "config_ac.h"
#include "arch.h"
#include "os_calls.h"
#include "parse.h"
#include "trans.h"
#include "libxrdpinc.h"
#include "xrdp_client_info.h"
#include "xrdp_constants.h"

// ----------------------------------------------------------------------------
// External plumbing
// ----------------------------------------------------------------------------

// rdpmac_log lives in RDPSubsystem.c (currently static; will be made non-static
// when RDPSubsystem.c is rewritten in phase 2d). For now we forward-declare it
// — if the symbol isn't visible at link time the build system will surface it.
void rdpmac_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Forward declaration of the Swift bridge context. The canonical definition
// lives in RDPSubsystem.h, but that header still drags in FreeRDP. The struct
// layout mirrors the one in RDPSubsystem.h exactly; phase 2d will consolidate
// the declarations.
typedef struct {
    void* swiftContext;
    void (*onCaptureStart)(void*);
    void (*onCaptureStop)(void*);
    void (*onKeyboardEvent)(void*, uint16_t, uint8_t);
    void (*onUnicodeKeyboardEvent)(void*, uint16_t, uint16_t);
    void (*onMouseEvent)(void*, uint16_t, uint16_t, uint16_t);
    void (*onClientConnect)(void*);
    void (*onClientDisconnect)(void*);
    void (*onClientResolution)(void*, uint32_t, uint32_t);
} RDPMacSubsystemContext;

extern RDPMacSubsystemContext g_macSubsystemContext;

// ----------------------------------------------------------------------------
// RDP message identifiers used by libxrdp's session callback
// (mirrors values in common/ms-rdpbcgr.h: RDP_INPUT_SCANCODE=4,
//  RDP_INPUT_UNICODE=5, RDP_INPUT_MOUSE=0x8001).
// ----------------------------------------------------------------------------

#ifndef RDP_INPUT_SCANCODE
#define RDP_INPUT_SCANCODE  4
#endif
#ifndef RDP_INPUT_UNICODE
#define RDP_INPUT_UNICODE   5
#endif
#ifndef RDP_INPUT_MOUSE
#define RDP_INPUT_MOUSE     0x8001
#endif

// "up and running" notification from libxrdp once the RDP handshake completes
#define LIBXRDP_MSG_UP_AND_RUNNING 0x555a

// SSL_PROTOCOLS_DEFAULT isn't exported in the public headers; libxrdp's own
// xrdp_sec.c picks the protocols internally when negotiation enables TLS.
// For our optional manual TLS-mode call we pass 0, which lets the helper use
// its default disable mask (matches what xrdp daemon does).
#ifndef SSL_PROTOCOLS_DEFAULT
#define SSL_PROTOCOLS_DEFAULT 0
#endif

// ----------------------------------------------------------------------------
// Internal types
// ----------------------------------------------------------------------------

struct rdpmac_client {
    struct rdpmac_listener* listener;
    struct trans*           trans;
    struct xrdp_session*    session;
    pthread_t               thread;
    atomic_int              should_stop;
    atomic_int              up_and_running;
    // Set to 1 by the up_and_running handler; cleared when the client has
    // received its first full-screen repaint. While set, push_frame ignores
    // ScreenCaptureKit's (possibly partial) dirty rects and pushes every
    // tile so the placeholder/blue first frame is overwritten promptly.
    atomic_int              needs_full_repaint;

    // DYNVC + Display Control state. `dynvc_started` flips to 1 once we've
    // called libxrdp_drdynvc_start; `disp_chan_id` is the dynamic channel
    // ID returned by libxrdp_drdynvc_open for "Microsoft::Windows::RDS::
    // DisplayControl" (-1 = not yet open). Both are touched only from the
    // libxrdp client thread, so no atomic discipline is needed.
    int                     dynvc_started;
    int                     disp_chan_id;

    // Per-client send lock. OpenSSL's SSL_write is NOT thread-safe on a
    // single SSL_CTX/SSL pair: if two threads call SSL_write concurrently,
    // the internal write-record state corrupts and we crash inside
    // tls_write_records_default (memmove). Both the capture thread (via
    // rdpmac_listener_push_frame → libxrdp_send_bitmap → trans_write →
    // SSL_write) and the client thread (via trans_check_wait_objs →
    // trans_send_waiting → SSL_write) can hit this simultaneously, so
    // we serialize all send-side trans access on this lock.
    pthread_mutex_t         send_mu;

    // Singly-linked intrusive list inside the listener
    struct rdpmac_client*   next;
};

struct rdpmac_listener {
    rdpmac_listener_config  cfg;            // owned copies of cert/key strings
    bool                    cfg_valid;
    char                    ini_path[1024]; // generated minimal xrdp.ini

    struct trans*           listener_trans;

    pthread_t               listener_thread;
    bool                    listener_thread_started;
    atomic_int              should_stop;

    pthread_mutex_t         clients_mu;
    struct rdpmac_client*   clients;        // head of singly-linked list
};

// Generates a minimal xrdp.ini under ~/Library/Application Support/RDPonMAC/
// libxrdp reads "[globals]" from this file in xrdp_rdp_read_config() to
// populate client_info defaults — particularly the cert/key paths,
// crypt_level, and security_layer that the licensing/encryption layer
// needs to send well-formed responses to strict clients (mstsc et al.).
static bool
write_minimal_xrdp_ini(const rdpmac_listener_config* cfg, char* out_path, size_t out_path_len)
{
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/Library/Application Support/RDPonMAC", home);
    mkdir(dir, 0700);
    snprintf(out_path, out_path_len, "%s/xrdp.ini", dir);

    FILE* f = fopen(out_path, "w");
    if (!f) {
        rdpmac_log("[xRDP] cannot create xrdp.ini at %s\n", out_path);
        return false;
    }

    // Match xrdp's own default xrdp.ini.in [globals] settings, with our
    // cert/key paths plugged in. crypt_level=low is the standard choice
    // when TLS is in use (the RDP-level encryption is then a no-op layer
    // wrapped inside TLS, which mstsc accepts).
    fprintf(f,
        "[globals]\n"
        "bitmap_cache=true\n"
        "bitmap_compression=true\n"
        "port=%u\n"
        "crypt_level=low\n"
        "security_layer=negotiate\n"
        "ssl_protocols=TLSv1.2, TLSv1.3\n"
        "tls_ciphers=HIGH:!aNULL:!eNULL:!EXPORT:!DES:!3DES:!RC4:!MD5\n"
        "allow_channels=true\n"
        "allow_multimon=true\n"
        "max_bpp=32\n"
        "new_cursors=true\n"
        "use_fastpath=both\n"
        "require_credentials=false\n",
        cfg->port);

    if (cfg->certFile && cfg->certFile[0]) {
        fprintf(f, "certificate=%s\n", cfg->certFile);
    }
    if (cfg->keyFile && cfg->keyFile[0]) {
        fprintf(f, "key_file=%s\n", cfg->keyFile);
    }

    fclose(f);
    rdpmac_log("[xRDP] wrote minimal xrdp.ini to %s\n", out_path);
    return true;
}

// ----------------------------------------------------------------------------
// Forward declarations
// ----------------------------------------------------------------------------

static void* listener_thread_main(void* arg);
static int   on_new_connection_callback(struct trans* self, struct trans* new_self);
static void* client_thread_main(void* arg);
static int   client_on_data_in(struct trans* self);
static int   client_xrdp_callback(intptr_t id, int msg,
                                  intptr_t param1, intptr_t param2,
                                  intptr_t param3, intptr_t param4);
static int   client_is_term(void);
static void  client_remove_from_listener(rdpmac_listener* l, rdpmac_client* c);

// Module-global pointer used by client_is_term() (xrdp's is_term hook takes
// no arguments, so we need a way to reach the listener's should_stop flag).
// There is exactly one listener per process so this is safe.
static rdpmac_listener* g_active_listener = NULL;

// ============================================================================
// Public API
// ============================================================================

rdpmac_listener*
rdpmac_listener_create(void)
{
    rdpmac_listener* l = (rdpmac_listener*)calloc(1, sizeof(*l));
    if (l == NULL) {
        rdpmac_log("[xRDP] rdpmac_listener_create: calloc failed\n");
        return NULL;
    }

    if (pthread_mutex_init(&l->clients_mu, NULL) != 0) {
        rdpmac_log("[xRDP] rdpmac_listener_create: pthread_mutex_init failed\n");
        free(l);
        return NULL;
    }

    atomic_init(&l->should_stop, 0);
    l->clients = NULL;
    l->listener_trans = NULL;
    l->listener_thread_started = false;
    l->cfg_valid = false;

    rdpmac_log("[xRDP] listener created (%p)\n", (void*)l);
    return l;
}

bool
rdpmac_listener_configure(rdpmac_listener* l, const rdpmac_listener_config* cfg)
{
    if (l == NULL || cfg == NULL) {
        return false;
    }

    // Free any previously-stored cert/key paths (defensive — configure is
    // typically called only once before start).
    if (l->cfg.certFile) { free(l->cfg.certFile); l->cfg.certFile = NULL; }
    if (l->cfg.keyFile)  { free(l->cfg.keyFile);  l->cfg.keyFile  = NULL; }

    l->cfg.port            = cfg->port ? cfg->port : 3389;
    l->cfg.authentication  = cfg->authentication;
    l->cfg.certFile        = cfg->certFile ? strdup(cfg->certFile) : NULL;
    l->cfg.keyFile         = cfg->keyFile  ? strdup(cfg->keyFile)  : NULL;

    if ((cfg->certFile && !l->cfg.certFile) ||
        (cfg->keyFile  && !l->cfg.keyFile)) {
        rdpmac_log("[xRDP] rdpmac_listener_configure: strdup failed\n");
        if (l->cfg.certFile) { free(l->cfg.certFile); l->cfg.certFile = NULL; }
        if (l->cfg.keyFile)  { free(l->cfg.keyFile);  l->cfg.keyFile  = NULL; }
        return false;
    }

    l->cfg_valid = true;

    // Generate the xrdp.ini libxrdp_init reads from. Without this, libxrdp
    // uses zero-defaults that cause the licensing/encryption layer to send
    // malformed responses (mstsc rejects with 0xc06 = NO_LICENSE).
    if (!write_minimal_xrdp_ini(&l->cfg, l->ini_path, sizeof(l->ini_path))) {
        l->ini_path[0] = '\0';
    }

    rdpmac_log("[xRDP] listener configured (port=%u, auth=%d, cert=%s, key=%s, ini=%s)\n",
               l->cfg.port, (int)l->cfg.authentication,
               l->cfg.certFile ? l->cfg.certFile : "(none)",
               l->cfg.keyFile  ? l->cfg.keyFile  : "(none)",
               l->ini_path[0] ? l->ini_path : "(none)");
    return true;
}

int
rdpmac_listener_start(rdpmac_listener* l)
{
    if (l == NULL) {
        return -1;
    }
    if (!l->cfg_valid) {
        rdpmac_log("[xRDP] rdpmac_listener_start: not configured\n");
        return -1;
    }
    if (l->listener_thread_started) {
        rdpmac_log("[xRDP] rdpmac_listener_start: already started\n");
        return -1;
    }

    // Module-global pointer for client_is_term().
    g_active_listener = l;

    // Create a TCP listener transport. xrdp uses 8K in/out buffers in the
    // accepting socket; the listener itself doesn't move data so the size is
    // mostly bookkeeping.
    l->listener_trans = trans_create(TRANS_MODE_TCP, 8192, 8192);
    if (l->listener_trans == NULL) {
        rdpmac_log("[xRDP] rdpmac_listener_start: trans_create failed\n");
        return -1;
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", l->cfg.port);

    if (trans_listen(l->listener_trans, port_str) != 0) {
        rdpmac_log("[xRDP] rdpmac_listener_start: trans_listen(%s) failed\n",
                   port_str);
        trans_delete(l->listener_trans);
        l->listener_trans = NULL;
        return -1;
    }

    l->listener_trans->trans_conn_in = on_new_connection_callback;
    l->listener_trans->callback_data = l;

    atomic_store(&l->should_stop, 0);

    if (pthread_create(&l->listener_thread, NULL, listener_thread_main, l) != 0) {
        rdpmac_log("[xRDP] rdpmac_listener_start: pthread_create failed\n");
        trans_delete(l->listener_trans);
        l->listener_trans = NULL;
        return -1;
    }
    l->listener_thread_started = true;

    rdpmac_log("[xRDP] listener bound to port %u\n", l->cfg.port);
    return 0;
}

int
rdpmac_listener_stop(rdpmac_listener* l)
{
    if (l == NULL) {
        return -1;
    }

    rdpmac_log("[xRDP] listener stopping...\n");
    atomic_store(&l->should_stop, 1);

    // Wake/join the listener thread first so no new clients are accepted.
    if (l->listener_thread_started) {
        pthread_join(l->listener_thread, NULL);
        l->listener_thread_started = false;
    }

    // Signal every client thread to stop, then join them. We do this in two
    // passes: first set the flag under the mutex (which only briefly blocks
    // the client threads as they remove themselves on exit), then walk the
    // list and join. Each client thread removes itself from the list during
    // its own teardown, so we keep popping the head.
    pthread_mutex_lock(&l->clients_mu);
    for (rdpmac_client* c = l->clients; c != NULL; c = c->next) {
        atomic_store(&c->should_stop, 1);
    }
    pthread_mutex_unlock(&l->clients_mu);

    while (1) {
        pthread_mutex_lock(&l->clients_mu);
        rdpmac_client* c = l->clients;
        pthread_mutex_unlock(&l->clients_mu);
        if (c == NULL) {
            break;
        }
        // Join this thread — once it returns, the thread has removed itself.
        pthread_join(c->thread, NULL);
    }

    rdpmac_log("[xRDP] listener stopped\n");
    return 0;
}

void
rdpmac_listener_free(rdpmac_listener* l)
{
    if (l == NULL) {
        return;
    }

    // Defensive — caller should have called stop already, but if not, do it
    // now so we don't leak threads.
    if (l->listener_thread_started || l->clients != NULL) {
        rdpmac_listener_stop(l);
    }

    if (l->listener_trans != NULL) {
        trans_delete(l->listener_trans);
        l->listener_trans = NULL;
    }

    if (l->cfg.certFile) free(l->cfg.certFile);
    if (l->cfg.keyFile)  free(l->cfg.keyFile);

    pthread_mutex_destroy(&l->clients_mu);

    if (g_active_listener == l) {
        g_active_listener = NULL;
    }

    rdpmac_log("[xRDP] listener freed (%p)\n", (void*)l);
    free(l);
}

int
rdpmac_listener_client_count(rdpmac_listener* l)
{
    if (l == NULL) {
        return 0;
    }
    int n = 0;
    pthread_mutex_lock(&l->clients_mu);
    for (rdpmac_client* c = l->clients; c != NULL; c = c->next) {
        n++;
    }
    pthread_mutex_unlock(&l->clients_mu);
    return n;
}

// ----------------------------------------------------------------------------
// Frame delivery — tiled libxrdp_send_bitmap.
//
// Why tiled: a single libxrdp_send_bitmap for a full Retina frame produces
// ~1 PDU per output row (slow-path TS_UPDATE_BITMAP, ≤16KB each). At 2560-wide
// that's ~1440 PDUs per frame, which mstsc rejects. By slicing into 64×64
// tiles, each tile fits in a single PDU. We then drive the tile selection
// from the dirty rects ScreenCaptureKit already gives us — only changed
// tiles get sent each frame, so the steady-state PDU rate is small.
// ----------------------------------------------------------------------------

#define TILE_SIZE              64
// Per-frame tile cap. Sized to cover a full 4K (3840x2160 = 60x34 = 2040
// tiles) repaint in a single frame, so window moves and large redraws don't
// take multiple frames to settle. mstsc empirically handles ~12K PDUs/s in
// burst (400 tiles × 30 FPS); we sustain the same average rate when idle
// because dirty rects there are small.
#define MAX_TILES_PER_FRAME    2400
// Largest source we'll accept. With 64-px tiles this caps the dedup bitmap
// at (5120/64) * (3072/64) bits = 80 * 48 = 3840 bits = 480 bytes on stack.
#define MAX_TILE_GRID_W        80
#define MAX_TILE_GRID_H        48

// Sum of bytes still queued in the trans's pending-output linked list.
// Each item is a stream that couldn't be sent immediately (kernel TCP send
// buffer was full). If this grows large the network or client is behind,
// and continuing to push frames just adds latency.
static size_t
trans_pending_bytes(const struct trans* t)
{
    if (t == NULL) return 0;
    size_t total = 0;
    const struct stream* s = t->wait_s;
    while (s != NULL) {
        total += (size_t)(s->end - s->data);
        s = s->next;
    }
    return total;
}

// Backpressure threshold: if a client has more than this many bytes pending
// in its trans output queue, skip sending more this frame. Sized so the
// queue can hold ~1 frame's worth of dirty area at native res before we
// start dropping, but never more than that — otherwise mstsc starts
// rendering with multi-second lag.
#define BACKPRESSURE_BYTES (4 * 1024 * 1024)  // 4 MB

// Send one BGRX32 tile via the slow-path bitmap update. Caller MUST hold
// the client's send_mu. Repacks `stride`-aligned source rows into a tightly
// packed `tw*4` scratch buffer because libxrdp_send_bitmap assumes width*Bpp.
static int
send_tile(struct xrdp_session* session,
          const uint8_t* data, uint32_t stride,
          int frame_w, int frame_h,
          int tx, int ty)
{
    int tw = (tx + TILE_SIZE > frame_w) ? (frame_w - tx) : TILE_SIZE;
    int th = (ty + TILE_SIZE > frame_h) ? (frame_h - ty) : TILE_SIZE;
    if (tw <= 0 || th <= 0) {
        return 0;
    }

    // Stack scratch (16KB max — well within thread stack limits).
    uint8_t scratch[TILE_SIZE * TILE_SIZE * 4];
    for (int row = 0; row < th; row++) {
        memcpy(scratch + (size_t)row * tw * 4,
               data + ((size_t)ty + row) * stride + (size_t)tx * 4,
               (size_t)tw * 4);
    }

    return libxrdp_send_bitmap(session,
                               tw, th, 32,
                               (char*)scratch,
                               tx, ty, tw, th);
}

void
rdpmac_listener_push_frame(rdpmac_listener* l,
                           const uint8_t* data,
                           uint32_t width, uint32_t height, uint32_t stride,
                           uint32_t numRects, const rdpmac_rect* rects)
{
    if (l == NULL || data == NULL || width == 0 || height == 0) {
        return;
    }

    // Fixed 30 FPS cadence (33 ms minimum interval). The MAX_TILES_PER_FRAME
    // cap below protects mstsc from burst overload on huge repaints — we'd
    // rather drop tiles than slow the frame rate, since stale tiles get
    // picked up by the rotating keep-alive band within ~250 ms anyway.
    static double last_push = 0.0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    if (now - last_push < 0.033) {
        return;
    }
    last_push = now;

    // Build a deduplicated tile bitmap. Each bit represents one TILE_SIZE×
    // TILE_SIZE tile in the frame. `numRects == 0` means full-frame redraw
    // (e.g. first frame after up_and_running, or no SCKit dirty rects).
    int tiles_w = ((int)width  + TILE_SIZE - 1) / TILE_SIZE;
    int tiles_h = ((int)height + TILE_SIZE - 1) / TILE_SIZE;
    if (tiles_w > MAX_TILE_GRID_W) tiles_w = MAX_TILE_GRID_W;
    if (tiles_h > MAX_TILE_GRID_H) tiles_h = MAX_TILE_GRID_H;
    int total_tiles = tiles_w * tiles_h;

    static const int BMP_BYTES = (MAX_TILE_GRID_W * MAX_TILE_GRID_H + 7) / 8;
    uint8_t dirty_bmp[BMP_BYTES];
    memset(dirty_bmp, 0, sizeof(dirty_bmp));

    int dirty_tiles = 0;

    // Periodic keep-alive: every ~2 seconds, mark a strip of tiles dirty
    // so areas the OS copy-blitted (and ScreenCaptureKit therefore didn't
    // report as dirty — e.g. the spot a window was just dragged from) get
    // a fresh repaint. We rotate through the screen in horizontal bands so
    // we cover everything within ~2-3 seconds without ever bursting more
    // than `band_height_in_tiles` tiles at once.
    static double last_refresh_pass = 0.0;
    static int    refresh_band_y    = 0;  // tile-row to refresh next
    bool force_band = false;
    if (now - last_refresh_pass > 0.250) {
        last_refresh_pass = now;
        force_band = true;
    }

    if (numRects == 0 || rects == NULL) {
        // Whole frame.
        memset(dirty_bmp, 0xFF, sizeof(dirty_bmp));
        dirty_tiles = total_tiles;
    } else {
        for (uint32_t r = 0; r < numRects; r++) {
            int x0 = rects[r].left   / TILE_SIZE;
            int y0 = rects[r].top    / TILE_SIZE;
            int x1 = (rects[r].right  + TILE_SIZE - 1) / TILE_SIZE;
            int y1 = (rects[r].bottom + TILE_SIZE - 1) / TILE_SIZE;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > tiles_w) x1 = tiles_w;
            if (y1 > tiles_h) y1 = tiles_h;
            for (int ty = y0; ty < y1; ty++) {
                for (int tx = x0; tx < x1; tx++) {
                    int idx = ty * tiles_w + tx;
                    int byte = idx >> 3;
                    int mask = 1 << (idx & 7);
                    if (!(dirty_bmp[byte] & mask)) {
                        dirty_bmp[byte] |= (uint8_t)mask;
                        dirty_tiles++;
                    }
                }
            }
        }
    }

    // Add the keep-alive band on top of any dirty rects from this frame.
    if (force_band) {
        if (refresh_band_y >= tiles_h) refresh_band_y = 0;
        for (int tx = 0; tx < tiles_w; tx++) {
            int idx = refresh_band_y * tiles_w + tx;
            int byte = idx >> 3;
            int mask = 1 << (idx & 7);
            if (!(dirty_bmp[byte] & mask)) {
                dirty_bmp[byte] |= (uint8_t)mask;
                dirty_tiles++;
            }
        }
        refresh_band_y++;
    }

    if (dirty_tiles == 0) {
        return;
    }
    if (dirty_tiles > MAX_TILES_PER_FRAME) {
        dirty_tiles = MAX_TILES_PER_FRAME;
    }

    static int frame_count = 0;
    static int active_frames = 0;
    int active_clients = 0;

    pthread_mutex_lock(&l->clients_mu);
    for (rdpmac_client* c = l->clients; c != NULL; c = c->next) {
        if (!atomic_load(&c->up_and_running) || c->session == NULL) {
            continue;
        }
        active_clients++;

        // Per-client view of dirty tiles. Starts as a copy of the frame's
        // dedup bitmap. If this client just transitioned to up_and_running,
        // override with all-ones so we overwrite the placeholder/blue
        // initial frame promptly even when SCKit reports few dirty rects.
        uint8_t client_bmp[BMP_BYTES];
        int client_dirty = dirty_tiles;
        bool full_repaint = atomic_exchange(&c->needs_full_repaint, 0);
        if (full_repaint) {
            memset(client_bmp, 0xFF, sizeof(client_bmp));
            client_dirty = total_tiles;
        } else {
            memcpy(client_bmp, dirty_bmp, sizeof(client_bmp));
        }

        // Backpressure: if the client's trans output queue is already
        // backed up, skip this frame entirely UNLESS we owe a full repaint
        // (in which case we wait for the queue to drain on a future call,
        // but keep the flag set). Without the override, a heavily-loaded
        // queue would persist the placeholder.
        size_t pending = trans_pending_bytes(c->trans);
        if (pending > BACKPRESSURE_BYTES) {
            if (full_repaint) {
                // Re-arm so a later, drained frame still does the repaint.
                atomic_store(&c->needs_full_repaint, 1);
            }
            if ((frame_count % 30) == 0) {
                rdpmac_log("[xRDP] push_frame: skip — pending=%zu MB\n",
                           pending / (1024 * 1024));
            }
            continue;
        }

        pthread_mutex_lock(&c->send_mu);
        int sent = 0;
        int fail = 0;
        bool dropped = false;
        // Inside the send loop, recheck backpressure every 32 tiles so a
        // huge dirty set can't push the queue beyond the threshold either.
        for (int idx = 0; idx < tiles_w * tiles_h && sent < client_dirty; idx++) {
            if (!(client_bmp[idx >> 3] & (1 << (idx & 7)))) {
                continue;
            }
            if ((sent & 31) == 0 && trans_pending_bytes(c->trans) > BACKPRESSURE_BYTES) {
                dropped = true;
                break;
            }
            int tx = (idx % tiles_w) * TILE_SIZE;
            int ty = (idx / tiles_w) * TILE_SIZE;
            int rv = send_tile(c->session, data, stride,
                               (int)width, (int)height, tx, ty);
            if (rv != 0) {
                fail++;
            }
            sent++;
        }
        pthread_mutex_unlock(&c->send_mu);

        // If the full repaint got cut short by backpressure, re-arm it for
        // the next frame so it eventually completes.
        if (full_repaint && dropped) {
            atomic_store(&c->needs_full_repaint, 1);
        }

        if (active_frames < 3 || (active_frames % 30) == 0 || fail > 0 ||
            full_repaint) {
            rdpmac_log("[xRDP] push_frame[total=%d, active=%d]: tiles_sent=%d "
                       "fails=%d full_repaint=%d (%ux%u, dirty=%d/%d, "
                       "pending=%zuKB)\n",
                       frame_count, active_frames, sent, fail,
                       (int)full_repaint, width, height,
                       client_dirty, total_tiles, pending / 1024);
        }
    }
    if (active_clients > 0) {
        active_frames++;
    }
    frame_count++;
    pthread_mutex_unlock(&l->clients_mu);
}

// ----------------------------------------------------------------------------
// Cursor (pointer) updates — push the current macOS cursor shape to every
// connected client. Called from CursorService.swift when NSCursor.current
// changes. Without this, the client renders a stale cursor (typically the
// system arrow we sent at up_and_running) and the user can't see resize/
// I-beam/hand cursors that hint at clickable or resizable UI.
// ----------------------------------------------------------------------------

void
rdpmac_listener_push_pointer(rdpmac_listener* l,
                             const uint8_t* rgba,
                             uint32_t width, uint32_t height,
                             uint32_t hotX, uint32_t hotY)
{
    if (l == NULL || rgba == NULL || width == 0 || height == 0) {
        return;
    }
    // Cap at 96x96 — the largest size supported by RDP_CAPSET_LARGE_POINTER.
    // Most cursors are 32x32; resize cursors a bit larger.
    if (width  > 96) width  = 96;
    if (height > 96) height = 96;

    // libxrdp expects bottom-up BGRA pixel data plus a 1-bpp mask whose bits
    // run left-to-right within each byte, top-down rows. Rebuild both from
    // the input top-down RGBA buffer.
    size_t pix_count = (size_t)width * height;
    uint8_t bgra[96 * 96 * 4];
    int mask_stride = ((int)width + 15) / 16 * 2;     // 16-bit aligned per spec
    uint8_t mask[96 * 16];                            // > worst-case 96/8 * 96
    memset(mask, 0xFF, sizeof(mask));                 // all-transparent default
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* src_row = rgba + (size_t)y * width * 4;
        // Bottom-up destination row for the BGRA buffer libxrdp emits.
        uint8_t* dst_row = bgra + (size_t)(height - 1 - y) * width * 4;
        uint8_t* mask_row = mask + (size_t)y * mask_stride;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t r = src_row[x*4 + 0];
            uint8_t g = src_row[x*4 + 1];
            uint8_t b = src_row[x*4 + 2];
            uint8_t a = src_row[x*4 + 3];
            dst_row[x*4 + 0] = b;
            dst_row[x*4 + 1] = g;
            dst_row[x*4 + 2] = r;
            dst_row[x*4 + 3] = a;
            // 1-bpp AND-mask: 0 = pixel is part of cursor, 1 = transparent.
            // We treat any pixel with alpha < 128 as transparent.
            int byte_idx = x / 8;
            int bit_idx  = 7 - (x % 8);
            if (a >= 128) {
                mask_row[byte_idx] &= (uint8_t)~(1 << bit_idx);
            }
        }
    }
    (void)pix_count;

    pthread_mutex_lock(&l->clients_mu);
    for (rdpmac_client* c = l->clients; c != NULL; c = c->next) {
        if (!atomic_load(&c->up_and_running) || c->session == NULL) {
            continue;
        }
        pthread_mutex_lock(&c->send_mu);
        // cache_idx 0 — overwrites the same cache slot. Modern clients
        // accept this; older ones may need a per-shape cache_idx, which
        // we'll add only if this proves insufficient.
        int rv = libxrdp_send_pointer(c->session, /*cache_idx*/ 0,
                                      (char*)bgra, (char*)mask,
                                      (int)hotX, (int)hotY,
                                      /*bpp*/ 32,
                                      (int)width, (int)height);
        pthread_mutex_unlock(&c->send_mu);
        if (rv != 0) {
            rdpmac_log("[xRDP] push_pointer: libxrdp_send_pointer rv=%d\n", rv);
        }
    }
    pthread_mutex_unlock(&l->clients_mu);
}

// ============================================================================
// Listener thread — accepts incoming connections
// ============================================================================

static void*
listener_thread_main(void* arg)
{
    rdpmac_listener* l = (rdpmac_listener*)arg;
    rdpmac_log("[xRDP] listener thread started\n");

    while (!atomic_load(&l->should_stop)) {
        tbus robjs[8];
        int  robjs_count = 0;

        if (l->listener_trans == NULL) {
            break;
        }

        if (trans_get_wait_objs(l->listener_trans, robjs, &robjs_count) != 0) {
            rdpmac_log("[xRDP] listener: trans_get_wait_objs failed\n");
            break;
        }

        // 1-second poll so we periodically observe should_stop.
        if (g_obj_wait(robjs, robjs_count, NULL, 0, 1000) != 0) {
            // Spurious / interrupted wait — just retry.
            continue;
        }

        if (atomic_load(&l->should_stop)) {
            break;
        }

        // trans_check_wait_objs invokes trans_conn_in for every accepted
        // socket; that callback (on_new_connection_callback) hands the new
        // trans to a fresh client thread.
        if (trans_check_wait_objs(l->listener_trans) != 0) {
            rdpmac_log("[xRDP] listener: trans_check_wait_objs failed\n");
            break;
        }
    }

    rdpmac_log("[xRDP] listener thread exiting\n");
    return NULL;
}

// Called by the trans layer whenever accept() returns a new socket. We're
// still on the listener thread here.
static int
on_new_connection_callback(struct trans* self, struct trans* new_self)
{
    rdpmac_listener* l = (rdpmac_listener*)(self->callback_data);
    if (l == NULL || new_self == NULL) {
        return 1;
    }

    rdpmac_client* client = (rdpmac_client*)calloc(1, sizeof(*client));
    if (client == NULL) {
        rdpmac_log("[xRDP] on_new_connection: calloc failed\n");
        trans_delete(new_self);
        return 1;
    }

    client->listener = l;
    client->trans    = new_self;
    client->session  = NULL;
    atomic_init(&client->should_stop, 0);
    atomic_init(&client->up_and_running, 0);
    atomic_init(&client->needs_full_repaint, 0);
    client->dynvc_started = 0;
    client->disp_chan_id  = -1;
    {
        // Recursive mutex: the client thread enters trans_check_wait_objs while
        // holding send_mu; libxrdp may invoke our session callback (e.g. on
        // up_and_running) which itself wants to call libxrdp_send_pointer
        // and re-lock send_mu. A non-recursive mutex deadlocks here.
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        int merr = pthread_mutex_init(&client->send_mu, &attr);
        pthread_mutexattr_destroy(&attr);
        if (merr != 0) {
            rdpmac_log("[xRDP] on_new_connection: send_mu init failed\n");
            free(client);
            trans_delete(new_self);
            return 1;
        }
    }

    // Push to head of client list.
    pthread_mutex_lock(&l->clients_mu);
    client->next = l->clients;
    l->clients   = client;
    pthread_mutex_unlock(&l->clients_mu);

    if (pthread_create(&client->thread, NULL, client_thread_main, client) != 0) {
        rdpmac_log("[xRDP] on_new_connection: pthread_create failed\n");
        // Pull ourselves back out of the list.
        client_remove_from_listener(l, client);
        trans_delete(new_self);
        free(client);
        return 1;
    }

    rdpmac_log("[xRDP] new client accepted (%p)\n", (void*)client);
    return 0;
}

// ============================================================================
// Client thread — one per connected RDP client
// ============================================================================

static void*
client_thread_main(void* arg)
{
    rdpmac_client*   client = (rdpmac_client*)arg;
    rdpmac_listener* l      = client->listener;

    rdpmac_log("[xRDP] client thread started (%p)\n", (void*)client);

    // Notify Swift that a client connected.
    if (g_macSubsystemContext.onClientConnect &&
        g_macSubsystemContext.swiftContext) {
        g_macSubsystemContext.onClientConnect(g_macSubsystemContext.swiftContext);
    }

    // Mirror xrdp_process_main_loop's setup in the documented order:
    //   1) callbacks first, 2) header_size/extra_flags zero, 3) init_stream
    //   for the early connect phase, 4) libxrdp_init, 5) post-init wiring.
    init_stream(client->trans->in_s, 8192 * 4);
    client->trans->no_stream_init_on_data_in = 1;
    client->trans->trans_data_in            = client_on_data_in;
    client->trans->callback_data            = client;
    client->trans->extra_flags              = 0;
    client->trans->header_size              = 0;

    const char* ini = (l->ini_path[0] != '\0') ? l->ini_path : NULL;
    client->session = libxrdp_init((tbus)client, client->trans, ini);
    if (client->session == NULL) {
        rdpmac_log("[xRDP] libxrdp_init failed (ini=%s)\n", ini ? ini : "NULL");
        goto cleanup;
    }

    // Plumb the configured cert/key paths into client_info so that
    // libxrdp's own TLS setup (in xrdp_iso.c during the negotiation phase)
    // finds them. Manual trans_set_tls_mode here is wrong — libxrdp does it
    // itself once the client requests TLS.
    if (l->cfg.certFile && client->session->client_info) {
        strncpy(client->session->client_info->certificate, l->cfg.certFile,
                sizeof(client->session->client_info->certificate) - 1);
    }
    if (l->cfg.keyFile && client->session->client_info) {
        strncpy(client->session->client_info->key_file, l->cfg.keyFile,
                sizeof(client->session->client_info->key_file) - 1);
    }

    client->trans->si        = &(client->session->si);
    client->trans->my_source = XRDP_SOURCE_CLIENT;
    client->session->callback = client_xrdp_callback;
    client->session->is_term  = client_is_term;

    if (libxrdp_process_incoming(client->session) != 0) {
        rdpmac_log("[xRDP] libxrdp_process_incoming failed\n");
        // Best-effort disconnect — same idiom as xrdp_process_main_loop.
        libxrdp_disconnect(client->session);
        goto cleanup;
    }

    // Switch to the larger steady-state buffer once the handshake is in.
    init_stream(client->trans->in_s, 32 * 1024);

    rdpmac_log("[xRDP] client handshake complete; entering main loop\n");

    // Main wait/process loop — mirrors xrdp_process_main_loop.
    while (!atomic_load(&client->should_stop) &&
           !atomic_load(&l->should_stop)) {
        tbus robjs[16];
        tbus wobjs[16];
        int  rcount  = 0;
        int  wcount  = 0;
        int  timeout = -1;

        if (trans_get_wait_objs_rw(client->trans, robjs, &rcount,
                                   wobjs, &wcount, &timeout) != 0) {
            rdpmac_log("[xRDP] client: trans_get_wait_objs_rw failed\n");
            break;
        }

        // Cap timeout so we observe should_stop quickly, but never block
        // indefinitely (timeout==-1 from trans means "no deadline").
        if (timeout < 0 || timeout > 100) {
            timeout = 100;
        }

        if (g_obj_wait(robjs, rcount, wobjs, wcount, timeout) != 0) {
            // Don't tight-loop on transient wait errors.
            g_sleep(10);
        }

        if (atomic_load(&client->should_stop) ||
            atomic_load(&l->should_stop)) {
            break;
        }

        // trans_check_wait_objs flushes any queued writes via trans_send_waiting
        // (which calls SSL_write). The capture thread also calls SSL_write via
        // libxrdp_send_bitmap. Both must serialize on send_mu — see struct doc.
        pthread_mutex_lock(&client->send_mu);
        int rv = trans_check_wait_objs(client->trans);
        pthread_mutex_unlock(&client->send_mu);
        if (rv != 0) {
            rdpmac_log("[xRDP] client trans_check_wait_objs returned %d "
                       "(status=%d, sck=%d)\n",
                       rv, (int)client->trans->status, (int)client->trans->sck);
            break;
        }
    }

    rdpmac_log("[xRDP] client main loop exited (should_stop=%d, listener_stop=%d)\n",
               (int)atomic_load(&client->should_stop),
               (int)atomic_load(&l->should_stop));
    libxrdp_disconnect(client->session);

cleanup:
    // CRITICAL ordering: remove from the listener's client list FIRST,
    // under the listener mutex, so that any in-flight push_frame iteration
    // either (a) hasn't reached this client yet — and won't, because it's
    // gone from the list — or (b) has already completed before we even got
    // here. Either way, by the time we free the session/trans below, no
    // other thread can be holding a stale pointer to them. Without this,
    // the screen-capture thread can dereference a session->client_info
    // that libxrdp_exit just freed → segfault inside libxrdp_send_bitmap.
    client_remove_from_listener(l, client);

    // Notify Swift after delisting (Swift just toggles UI state, doesn't
    // touch the session) so the GUI updates promptly.
    if (g_macSubsystemContext.onClientDisconnect &&
        g_macSubsystemContext.swiftContext) {
        g_macSubsystemContext.onClientDisconnect(g_macSubsystemContext.swiftContext);
    }

    if (client->session != NULL) {
        libxrdp_exit(client->session);
        client->session = NULL;
    }

    if (client->trans != NULL) {
        trans_delete(client->trans);
        client->trans = NULL;
    }

    pthread_mutex_destroy(&client->send_mu);
    rdpmac_log("[xRDP] client thread done (%p)\n", (void*)client);
    free(client);
    return NULL;
}

// trans data-in callback — invoked by trans_check_wait_objs each time
// `header_size` more bytes have been read into self->in_s. Implements the
// same per-PDU progressive-read state machine as xrdp_process_data_in
// (xrdp/xrdp_process.c). Stages:
//   extra_flags == 0  : connection sequence — let libxrdp pull bytes itself.
//   extra_flags == 1  : just buffered 2 bytes; figure out TPKT vs FastPath.
//   extra_flags == 2  : have enough bytes to compute the full PDU length.
//   extra_flags == 3  : full PDU buffered; hand it to libxrdp_process_data.
// After processing one PDU, reset to (header_size=2, extra_flags=1) and
// re-init the buffer so the next PDU starts cleanly.
static int
client_on_data_in(struct trans* self)
{
    rdpmac_client* client = (rdpmac_client*)(self->callback_data);
    if (client == NULL || client->session == NULL) {
        return 1;
    }

    struct stream* s = self->in_s;
    int len = 0;

    switch (self->extra_flags) {
        case 0:
            // Early connect sequence — libxrdp pulls bytes via its own
            // libxrdp_force_read path. We only call libxrdp_process_data
            // here (with NULL stream) to drive the state machine forward.
            if (libxrdp_process_data(client->session, NULL) != 0) {
                rdpmac_log("[xRDP] process_data (case 0) returned non-zero\n");
                return 1;
            }
            if (client->session->up_and_running) {
                self->header_size = 2;
                self->extra_flags = 1;
                init_stream(s, 0);
            }
            break;

        case 1:
            // We have 2 bytes; decide framing format.
            if ((unsigned char)s->p[0] == 3) {
                // TPKT — read 4-byte header next.
                self->header_size = 4;
                self->extra_flags = 2;
            } else {
                // FastPath — length lives in byte[1] (1- or 2-byte form).
                if ((unsigned char)s->p[1] & 0x80) {
                    self->header_size = 3;
                    self->extra_flags = 2;
                } else {
                    self->header_size = (unsigned char)s->p[1];
                    self->extra_flags = 3;
                }
            }
            len = (int)(s->end - s->data);
            if ((int)self->header_size > len) {
                /* not enough data read yet */
                break;
            }
            /* FALLTHROUGH */

        case 2:
            // We have enough header to compute the full PDU length.
            len = libxrdp_get_pdu_bytes(s->p);
            if (len == -1) {
                rdpmac_log("[xRDP] libxrdp_get_pdu_bytes failed\n");
                return 1;
            }
            self->header_size = len;
            self->extra_flags = 3;

            len = (int)(s->end - s->data);
            if ((int)self->header_size > len) {
                /* not enough data read yet */
                break;
            }
            /* FALLTHROUGH */

        case 3: {
            // Full PDU ready — hand to libxrdp.
            s->p = s->data;
            int pdu_len = (int)(s->end - s->data);
            int rv3 = libxrdp_process_data(client->session, s);
            if (rv3 != 0) {
                int dump_len = pdu_len < 32 ? pdu_len : 32;
                char hex[65];
                for (int k = 0; k < dump_len; k++) {
                    snprintf(hex + k*2, 3, "%02x", (unsigned char)s->data[k]);
                }
                hex[dump_len*2] = '\0';
                rdpmac_log("[xRDP] libxrdp_process_data (case 3) rv=%d "
                           "pdu_len=%d up=%d in_proc=%d head=%s\n",
                           rv3, pdu_len,
                           client->session->up_and_running,
                           (int)client->session->in_process_data, hex);
                return 1;
            }
        }
            init_stream(s, 0);
            self->header_size = 2;
            self->extra_flags = 1;
            break;
    }

    return 0;
}

// ----------------------------------------------------------------------------
// DYNVC + Display Control (MS-RDPEDISP) — lets mstsc/Jump/Windows App tell us
// "the user just resized my window, please re-negotiate the desktop at the
// new pixel size". We open the dynamic channel
// "Microsoft::Windows::RDS::DisplayControl" once, send a CAPS PDU declaring
// our limits, and then translate any incoming MONITOR_LAYOUT PDU into a
// client_info update + libxrdp_reset(). The reset triggers a Deactivate-All
// → Demand Active cycle which fires our up_and_running handler again with
// the new size, which already routes through onClientResolution → Swift,
// which live-updates SCStream and the input scaling.
// ----------------------------------------------------------------------------

#define DISPLAYCONTROL_PDU_TYPE_MONITOR_LAYOUT  0x00000002
#define DISPLAYCONTROL_PDU_TYPE_CAPS            0x00000005
#define DISPLAYCONTROL_MONITOR_PRIMARY          0x00000001

#define DISPLAYCONTROL_NAME "Microsoft::Windows::RDS::DisplayControl"

// Pack `v` as 4-byte little-endian into `dst`.
static inline void put_u32_le(uint8_t* dst, uint32_t v) {
    dst[0] = (uint8_t)(v        & 0xFF);
    dst[1] = (uint8_t)((v >>  8) & 0xFF);
    dst[2] = (uint8_t)((v >> 16) & 0xFF);
    dst[3] = (uint8_t)((v >> 24) & 0xFF);
}
// Read 4-byte little-endian from `src`.
static inline uint32_t get_u32_le(const uint8_t* src) {
    return (uint32_t)src[0]
         | ((uint32_t)src[1] <<  8)
         | ((uint32_t)src[2] << 16)
         | ((uint32_t)src[3] << 24);
}

// Send the server-to-client DISPLAYCONTROL_CAPS_PDU: 8-byte header + 12-byte
// body advertising max 1 monitor up to 8192×8192. Most clients ignore the
// values and just accept any layout, but mstsc requires the message before
// it sends a MONITOR_LAYOUT.
static int
disp_send_caps(rdpmac_client* client)
{
    uint8_t buf[20];
    put_u32_le(buf + 0,  DISPLAYCONTROL_PDU_TYPE_CAPS);   // Type
    put_u32_le(buf + 4,  20);                              // Length
    put_u32_le(buf + 8,  1);                               // MaxNumMonitors
    put_u32_le(buf + 12, 8192);                            // MaxMonitorAreaFactorA
    put_u32_le(buf + 16, 8192);                            // MaxMonitorAreaFactorB
    return libxrdp_drdynvc_data(client->session,
                                client->disp_chan_id,
                                (const char*)buf, sizeof(buf));
}

static int
disp_open_response(intptr_t id, int chan_id, int creation_status)
{
    rdpmac_client* client = (rdpmac_client*)id;
    rdpmac_log("[xRDP] DisplayControl channel open response: chan=%d "
               "status=%d\n", chan_id, creation_status);
    if (!client) return 0;
    if (creation_status != 0) {
        // Client doesn't support DisplayControl (or refused). Disable.
        client->disp_chan_id = -1;
        return 0;
    }
    int rv = disp_send_caps(client);
    rdpmac_log("[xRDP] DisplayControl caps sent rv=%d\n", rv);
    return 0;
}

static int
disp_close_response(intptr_t id, int chan_id)
{
    rdpmac_client* client = (rdpmac_client*)id;
    rdpmac_log("[xRDP] DisplayControl channel closed (chan=%d)\n", chan_id);
    if (client && client->disp_chan_id == chan_id) {
        client->disp_chan_id = -1;
    }
    return 0;
}

static int
disp_data_first(intptr_t id, int chan_id, char* data,
                int bytes, int total_bytes)
{
    // DisplayControl messages are small (≤80 bytes for a single-monitor
    // layout, growing linearly per monitor). They normally arrive in one
    // shot; if we ever see fragmentation we'd need a per-channel reassembly
    // buffer. For now log + ignore.
    (void)id; (void)chan_id; (void)data;
    rdpmac_log("[xRDP] DisplayControl data_first %d/%d bytes — "
               "fragmented messages not supported, dropping\n",
               bytes, total_bytes);
    return 0;
}

static int
disp_data(intptr_t id, int chan_id, char* data, int bytes)
{
    rdpmac_client* client = (rdpmac_client*)id;
    (void)chan_id;
    if (!client || !client->session || !client->session->client_info) {
        return 0;
    }
    if (bytes < 8) {
        rdpmac_log("[xRDP] DisplayControl data too short: %d\n", bytes);
        return 0;
    }

    const uint8_t* p = (const uint8_t*)data;
    uint32_t type = get_u32_le(p + 0);
    uint32_t len  = get_u32_le(p + 4);
    (void)len;

    if (type != DISPLAYCONTROL_PDU_TYPE_MONITOR_LAYOUT) {
        // CAPS PDUs are server→client only — we don't expect to receive one.
        rdpmac_log("[xRDP] DisplayControl unexpected PDU type=0x%x len=%u\n",
                   type, len);
        return 0;
    }

    // Body of MONITOR_LAYOUT_PDU (after the 8-byte header):
    //   u32 MonitorLayoutSize  (must be 40)
    //   u32 NumMonitors
    //   { 40-byte monitor records } * NumMonitors
    if (bytes < 8 + 8 + 40) {
        rdpmac_log("[xRDP] MONITOR_LAYOUT body too short: %d\n", bytes);
        return 0;
    }
    uint32_t monitor_layout_size = get_u32_le(p + 8);
    uint32_t num_monitors        = get_u32_le(p + 12);
    if (monitor_layout_size != 40 || num_monitors == 0) {
        rdpmac_log("[xRDP] MONITOR_LAYOUT bad: layoutSize=%u num=%u\n",
                   monitor_layout_size, num_monitors);
        return 0;
    }

    // Walk the monitor records, prefer the one flagged primary; fall back
    // to the first monitor.
    uint32_t new_w = 0, new_h = 0;
    bool got_primary = false;
    const uint8_t* m = p + 16;
    for (uint32_t i = 0; i < num_monitors; i++) {
        if ((size_t)(m - p) + 40 > (size_t)bytes) break;
        uint32_t flags  = get_u32_le(m + 0);
        // m+4..7 = Left, m+8..11 = Top
        uint32_t width  = get_u32_le(m + 12);
        uint32_t height = get_u32_le(m + 16);
        // physical_w/h, orientation, scale factors at +20..+39 — unused.
        if (!got_primary) {
            new_w = width;
            new_h = height;
        }
        if (flags & DISPLAYCONTROL_MONITOR_PRIMARY) {
            new_w = width;
            new_h = height;
            got_primary = true;
            break;
        }
        m += 40;
    }

    // Sanity-clamp: MS-RDPEDISP says 200..8192, RDP libxrdp side has the
    // same bounds. Reject out-of-range to avoid feeding garbage to reset.
    if (new_w < 200 || new_h < 200 ||
        new_w > 8192 || new_h > 8192) {
        rdpmac_log("[xRDP] MONITOR_LAYOUT size out of range %ux%u — ignoring\n",
                   new_w, new_h);
        return 0;
    }

    struct xrdp_client_info* ci = client->session->client_info;
    if (new_w == ci->display_sizes.session_width &&
        new_h == ci->display_sizes.session_height) {
        // Same size — nothing to do. Some clients re-send the same layout
        // multiple times during a single drag.
        return 0;
    }

    rdpmac_log("[xRDP] MONITOR_LAYOUT %ux%u → %ux%u; resetting session\n",
               ci->display_sizes.session_width,
               ci->display_sizes.session_height,
               new_w, new_h);

    // Update client_info BEFORE libxrdp_reset — the new Demand Active that
    // reset emits reads session_width/height straight out of client_info.
    ci->display_sizes.session_width  = new_w;
    ci->display_sizes.session_height = new_h;

    atomic_store(&client->needs_full_repaint, 1);
    int rv = libxrdp_reset(client->session);
    rdpmac_log("[xRDP] libxrdp_reset rv=%d (new size %ux%u)\n",
               rv, new_w, new_h);
    // libxrdp_reset triggers a fresh Deactivate-All → Demand Active cycle.
    // The client's response (TS_FONT_LIST_PDU) re-fires our up_and_running
    // handler with the new client_info, which calls onClientResolution →
    // Swift live-updates SCStream and input scaling. Capture stays running
    // through the reset; the next frame from SCStream just arrives at the
    // new dimensions.
    return 0;
}

static struct xrdp_drdynvc_procs disp_procs = {
    .open_response  = disp_open_response,
    .close_response = disp_close_response,
    .data_first     = disp_data_first,
    .data           = disp_data,
};

// libxrdp's session callback — input events and lifecycle notifications.
// Forwarded into the Swift bridge via g_macSubsystemContext.
static int
client_xrdp_callback(intptr_t id, int msg,
                     intptr_t param1, intptr_t param2,
                     intptr_t param3, intptr_t param4)
{
    rdpmac_client* client = (rdpmac_client*)id;
    (void)param4;

    switch (msg) {
        case RDP_INPUT_SCANCODE:
            // p1 = keycode, p3 = flags
            if (g_macSubsystemContext.onKeyboardEvent &&
                g_macSubsystemContext.swiftContext) {
                g_macSubsystemContext.onKeyboardEvent(
                    g_macSubsystemContext.swiftContext,
                    (uint16_t)param3,
                    (uint8_t)param1);
            }
            break;

        case RDP_INPUT_UNICODE:
            // p1 = unicode codepoint, p3 = flags
            if (g_macSubsystemContext.onUnicodeKeyboardEvent &&
                g_macSubsystemContext.swiftContext) {
                g_macSubsystemContext.onUnicodeKeyboardEvent(
                    g_macSubsystemContext.swiftContext,
                    (uint16_t)param3,
                    (uint16_t)param1);
            }
            break;

        case RDP_INPUT_MOUSE:
            // p1 = x, p2 = y, p3 = flags (RDP-native order)
            if (g_macSubsystemContext.onMouseEvent &&
                g_macSubsystemContext.swiftContext) {
                g_macSubsystemContext.onMouseEvent(
                    g_macSubsystemContext.swiftContext,
                    (uint16_t)param3,
                    (uint16_t)param1,
                    (uint16_t)param2);
            }
            break;

        case LIBXRDP_MSG_UP_AND_RUNNING:
            rdpmac_log("[xRDP] client up and running (%p)\n", (void*)client);
            if (client && client->session && client->session->client_info) {
                struct xrdp_client_info* ci = client->session->client_info;
                rdpmac_log("[xRDP] caps: bpp=%d use_bitmap_comp=%d "
                           "use_bitmap_cache=%d use_fast_path=%d "
                           "rfx=%d jpeg=%d h264=%d gfx=%d "
                           "desktop=%ux%u\n",
                           ci->bpp, ci->use_bitmap_comp, ci->use_bitmap_cache,
                           ci->use_fast_path, ci->rfx_codec_id,
                           ci->jpeg_codec_id, ci->h264_codec_id,
                           ci->use_bulk_comp,
                           ci->display_sizes.session_width,
                           ci->display_sizes.session_height);
                atomic_store(&client->up_and_running, 1);
                atomic_store(&client->needs_full_repaint, 1);
                pthread_mutex_lock(&client->send_mu);
                int prv = libxrdp_send_pointer_system(client->session, 0);

                pthread_mutex_unlock(&client->send_mu);
                rdpmac_log("[xRDP] sent system pointer rv=%d\n", prv);

                // Notify Swift of the negotiated desktop resolution so it
                // can downsample the ScreenCaptureKit output to match. The
                // client is already locked into ci->display_sizes.session_*
                // — libxrdp advertised those in Demand Active and the
                // client accepted in Confirm Active. Sending tiles outside
                // that surface gets clipped/discarded, so we may as well
                // capture at the right size.
                uint32_t cw = ci->display_sizes.session_width;
                uint32_t ch = ci->display_sizes.session_height;
                if (g_macSubsystemContext.onClientResolution &&
                    g_macSubsystemContext.swiftContext &&
                    cw > 0 && ch > 0) {
                    g_macSubsystemContext.onClientResolution(
                        g_macSubsystemContext.swiftContext,
                        cw, ch);
                }

                // Bring up DYNVC (the static "drdynvc" channel that hosts
                // dynamic virtual channels) and open the DisplayControl
                // dynamic channel so the client can tell us about live
                // window resizes. Only do this once per connection — every
                // libxrdp_reset re-fires up_and_running, but DYNVC stays
                // up and we don't want to re-open the channel.
                if (!client->dynvc_started) {
                    int dvc_rv = libxrdp_drdynvc_start(client->session);
                    rdpmac_log("[xRDP] libxrdp_drdynvc_start rv=%d\n", dvc_rv);
                    if (dvc_rv == 0) {
                        client->dynvc_started = 1;
                        int chan = -1;
                        int oo_rv = libxrdp_drdynvc_open(client->session,
                                                         DISPLAYCONTROL_NAME,
                                                         0,
                                                         &disp_procs,
                                                         &chan);
                        rdpmac_log("[xRDP] DisplayControl open rv=%d "
                                   "chan=%d\n", oo_rv, chan);
                        if (oo_rv == 0) {
                            client->disp_chan_id = chan;
                            // disp_open_response will fire when the client
                            // accepts the create request, and it sends the
                            // CAPS PDU from there.
                        }
                    }
                }
            }
            break;

        default:
            // Unhandled messages (channel data, sync, suppress-output, etc.)
            // — explicitly ignored for Phase 2.
            break;
    }
    return 0;
}

// libxrdp uses this as the global is-term hook. We hand back the listener's
// stop flag so libxrdp_process_data internals know to bail out on shutdown.
static int
client_is_term(void)
{
    rdpmac_listener* l = g_active_listener;
    if (l == NULL) {
        return 0;
    }
    return atomic_load(&l->should_stop) ? 1 : 0;
}

// ----------------------------------------------------------------------------
// Client list maintenance
// ----------------------------------------------------------------------------

static void
client_remove_from_listener(rdpmac_listener* l, rdpmac_client* c)
{
    if (l == NULL || c == NULL) {
        return;
    }

    pthread_mutex_lock(&l->clients_mu);
    rdpmac_client** pp = &l->clients;
    while (*pp != NULL) {
        if (*pp == c) {
            *pp = c->next;
            c->next = NULL;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&l->clients_mu);
}
