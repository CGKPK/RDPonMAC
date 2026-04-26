#include "RDPServerBridge.h"
#include "RDPSubsystem.h"
#include <freerdp/settings.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <winpr/ssl.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>

static void bridge_log(const char* fmt, ...) {
    FILE* f = fopen("/tmp/rdponmac.log", "a");
    if (!f) { f = stderr; }
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    if (f != stderr) { fflush(f); fclose(f); }
}

rdpShadowServer* rdp_server_create(void)
{
    // Enable FreeRDP debug logging to file
    setenv("WLOG_LEVEL", "DEBUG", 1);
    setenv("WLOG_FILEAPPENDER_OUTPUT_FILE_PATH", "/tmp/freerdp_server.log", 1);

    // Initialize WinPR and OpenSSL
    winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);

    // Register our custom subsystem entry before creating the server
    rdpmac_register_subsystem_entry();

    return shadow_server_new();
}

bool rdp_server_configure(rdpShadowServer* server,
                          uint32_t port,
                          const char* certFile,
                          const char* keyFile,
                          bool authentication)
{
    if (!server)
        return false;

    server->port = port;
    server->authentication = authentication;
    server->mayView = TRUE;
    server->mayInteract = TRUE;

    if (certFile) {
        free(server->CertificateFile);
        server->CertificateFile = _strdup(certFile);
    }
    if (keyFile) {
        free(server->PrivateKeyFile);
        server->PrivateKeyFile = _strdup(keyFile);
    }

    return true;
}

int rdp_server_init(rdpShadowServer* server)
{
    if (!server)
        return -1;

    // =========================================================
    // PHASE 1: Configure settings BEFORE shadow_server_init()
    // shadow_server_init() internally calls shadow_server_init_certificate()
    // which reads these settings. If we set them after, TLS won't be properly
    // initialized and negotiation fails for strict clients (Jump Desktop, Windows App).
    // =========================================================
    rdpSettings* settings = server->settings;
    if (settings) {
        // Security: TLS only — RDP security causes licensing and MCS issues
        freerdp_settings_set_bool(settings, FreeRDP_ServerMode, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_NegotiateSecurityLayer, TRUE);
        freerdp_settings_set_uint32(settings, FreeRDP_TlsSecLevel, 1);

        // Pre-load certificate into settings before shadow_server_init_certificate runs.
        // Note: shadow_server_init_certificate() will overwrite server->CertificateFile/PrivateKeyFile
        // with its own paths, but if the cert is already in settings, it will use ours.
        if (server->CertificateFile && server->PrivateKeyFile) {
            bridge_log("[RDPBridge] Cert path: %s\n", server->CertificateFile);
            bridge_log("[RDPBridge] Key path:  %s\n", server->PrivateKeyFile);
            rdpCertificate* cert = freerdp_certificate_new_from_file(server->CertificateFile);
            rdpPrivateKey* key = cert ? freerdp_key_new_from_file(server->PrivateKeyFile) : NULL;
            if (cert && key) {
                freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1);
                freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1);
                bridge_log("[RDPBridge] Cert pre-loaded OK\n");
            } else {
                bridge_log("[RDPBridge] Cert pre-load failed, will auto-generate\n");
                if (cert) freerdp_certificate_free(cert);
                if (key) freerdp_key_free(key);
            }
        }
    }

    // =========================================================
    // PHASE 2: Run shadow_server_init (subsystem, screen, listeners)
    // =========================================================
    int status = shadow_server_init(server);
    if (status < 0) {
        bridge_log("[RDPBridge] shadow_server_init failed: %d\n", status);
        return status;
    }

    // =========================================================
    // PHASE 3: Re-assert settings AFTER init (init may override some)
    // =========================================================
    settings = server->settings;
    if (settings) {
        // Log what init set
        bridge_log("[RDPBridge] After init: TLS=%d RDP=%d NLA=%d Negotiate=%d\n",
                freerdp_settings_get_bool(settings, FreeRDP_TlsSecurity),
                freerdp_settings_get_bool(settings, FreeRDP_RdpSecurity),
                freerdp_settings_get_bool(settings, FreeRDP_NlaSecurity),
                freerdp_settings_get_bool(settings, FreeRDP_NegotiateSecurityLayer));

        const rdpCertificate* postCert = freerdp_settings_get_pointer(settings, FreeRDP_RdpServerCertificate);
        const rdpPrivateKey* postKey = freerdp_settings_get_pointer(settings, FreeRDP_RdpServerRsaKey);
        bridge_log("[RDPBridge] After init: cert=%p key=%p\n", (const void*)postCert, (const void*)postKey);

        // Re-assert TLS-only settings
        freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_NegotiateSecurityLayer, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_ServerLicenseRequired, FALSE);

        // Codecs
        freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_NSCodec, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
        freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
    }

    return status;
}

int rdp_server_start(rdpShadowServer* server)
{
    if (!server)
        return -1;
    return shadow_server_start(server);
}

int rdp_server_stop(rdpShadowServer* server)
{
    if (!server)
        return -1;
    return shadow_server_stop(server);
}

int rdp_server_uninit(rdpShadowServer* server)
{
    if (!server)
        return -1;
    return shadow_server_uninit(server);
}

void rdp_server_free(rdpShadowServer* server)
{
    if (server)
        shadow_server_free(server);
}

rdpShadowSurface* rdp_server_get_surface(rdpShadowServer* server)
{
    if (!server)
        return NULL;
    return server->surface;
}

rdpSettings* rdp_server_get_settings(rdpShadowServer* server)
{
    if (!server)
        return NULL;
    return server->settings;
}
