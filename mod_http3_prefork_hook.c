/*
 * Copyright 2026 The mod_http3 Project Authors. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Experimental: QUIC integration with prefork MPM via accept_function hooks
 */

#include <httpd.h>
#include <http_config.h>
#include <http_connection.h>
#include <http_log.h>
#include <http_protocol.h>
#include <ap_listen.h>
#include <apr_strings.h>
#include <apr_hash.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <nghttp3/nghttp3.h>

#include "ossl-nghttp3.h"

module AP_MODULE_DECLARE_DATA http3_module;

/* ====================================================================
 * Data Structures
 * ==================================================================== */

/**
 * QUIC listener data - one per UDP socket
 */
typedef struct quic_listener_t {
    SSL_CTX *ssl_ctx;           /* OpenSSL QUIC context */
    SSL *ssl_listener;          /* OpenSSL listener object */
    apr_pool_t *pool;           /* Pool for this listener */
    server_rec *server;         /* Server config */
    const char *cert_path;
    const char *key_path;
    apr_hash_t *connections;    /* Active QUIC connections by SSL* */
} quic_listener_t;

/**
 * QUIC connection state - stored in conn_rec->conn_config
 */
typedef struct quic_conn_state_t {
    SSL *ssl_conn;              /* QUIC connection object */
    SSL *ssl_stream;            /* Current QUIC stream */
    apr_uint64_t stream_id;     /* Stream ID */
    nghttp3_conn *h3_conn;      /* HTTP/3 connection */
    apr_pool_t *pool;           /* Connection pool */
    quic_listener_t *listener;  /* Back pointer to listener */
} quic_conn_state_t;

/* ====================================================================
 * QUIC Accept Function - Called by Prefork MPM
 * ==================================================================== */

/**
 * Accept a QUIC stream and return it as a conn_rec
 *
 * This is the callback function registered with ap_listen_rec.
 * Prefork MPM calls this when the UDP socket is ready.
 */
static apr_status_t ap_quic_accept(void **accepted,
                                    ap_listen_rec *lr,
                                    apr_pool_t *ptrans)
{
    quic_listener_t *ql;
    SSL *conn = NULL;
    SSL *stream = NULL;

    /* Retrieve QUIC listener data from socket */
    apr_socket_data_get((void **)&ql, "quic_listener", lr->sd);

    if (!ql || !ql->ssl_listener) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, NULL,
                     "ap_quic_accept: invalid listener data");
        return APR_EGENERAL;
    }

    *accepted = NULL;

    /* Handle timeout events */
    SSL_handle_events(ql->ssl_listener);

    /* Try to accept new QUIC connections */
    while ((conn = SSL_accept_connection(ql->ssl_listener,
                                         SSL_ACCEPT_STREAM_NO_BLOCK))) {

        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ql->server,
                     "ap_quic_accept: accepted new QUIC connection %pp", conn);

        /* Store connection in hash table */
        apr_hash_set(ql->connections, &conn, sizeof(conn), conn);

        /* Try to accept a stream from this connection */
        stream = SSL_accept_stream(conn, SSL_ACCEPT_STREAM_NO_BLOCK);
        if (stream) {
            goto got_stream;
        }
    }

    /* Check existing connections for new streams */
    apr_hash_index_t *hi;
    for (hi = apr_hash_first(ptrans, ql->connections); hi; hi = apr_hash_next(hi)) {
        apr_hash_this(hi, NULL, NULL, (void**)&conn);

        stream = SSL_accept_stream(conn, SSL_ACCEPT_STREAM_NO_BLOCK);
        if (stream) {
            goto got_stream;
        }
    }

    /* No streams available */
    return APR_EAGAIN;

got_stream:
    /* Allocate and return QUIC stream wrapper */
    {
        quic_conn_state_t *qcs;
        apr_uint64_t stream_id = SSL_get_stream_id(stream);

        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ql->server,
                     "ap_quic_accept: accepted stream %" APR_UINT64_T_FMT
                     " from connection %pp",
                     stream_id, conn);

        /* Create QUIC connection state to pass back as "csd" */
        qcs = apr_pcalloc(ptrans, sizeof(*qcs));
        qcs->ssl_conn = conn;
        qcs->ssl_stream = stream;
        qcs->stream_id = stream_id;
        qcs->pool = ptrans;
        qcs->listener = ql;
        qcs->h3_conn = NULL;  /* TODO: Initialize nghttp3 */

        /* Return the QUIC state as the "connection socket descriptor" */
        /* Prefork MPM will pass this to ap_run_create_connection */
        *accepted = qcs;
        return APR_SUCCESS;
    }
}

/* ====================================================================
 * Listener Registration
 * ==================================================================== */

/**
 * Create SSL context for QUIC listener
 */
static SSL_CTX* create_quic_ssl_ctx(const char *cert_path,
                                     const char *key_path,
                                     server_rec *s)
{
    SSL_CTX *ctx = SSL_CTX_new(OSSL_QUIC_server_method());
    if (!ctx) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "Failed to create SSL_CTX: %s",
                     ERR_error_string(ERR_get_error(), NULL));
        return NULL;
    }

    /* Load certificate and key */
    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) <= 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "Failed to load certificate: %s", cert_path);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "Failed to load private key: %s", key_path);
        SSL_CTX_free(ctx);
        return NULL;
    }

    /* Set ALPN to h3 */
    const unsigned char alpn[] = "\x02h3";
    SSL_CTX_set_alpn_protos(ctx, alpn, sizeof(alpn) - 1);

    return ctx;
}

/**
 * Register QUIC listener with Apache
 * Called during pre_config
 */
static int quic_register_listener(apr_pool_t *pconf,
                                   server_rec *s,
                                   apr_uint16_t port,
                                   const char *cert_path,
                                   const char *key_path)
{
    apr_socket_t *udp_sock;
    apr_sockaddr_t *bind_addr;
    apr_status_t rv;
    quic_listener_t *ql;
    ap_listen_rec *lr;
    int fd;
    int on = 1;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                 "Registering QUIC listener on port %d", port);

    /* Create bind address */
    rv = apr_sockaddr_info_get(&bind_addr, NULL, APR_INET6, port, 0, pconf);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "Failed to create bind address for port %d", port);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Create UDP socket */
    rv = apr_socket_create(&udp_sock, APR_INET6, SOCK_DGRAM,
                           APR_PROTO_UDP, pconf);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "Failed to create UDP socket");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Set SO_REUSEPORT for multi-process support */
    apr_os_sock_get(&fd, udp_sock);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) < 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, apr_get_os_error(), s,
                     "Failed to set SO_REUSEPORT on UDP socket");
        apr_socket_close(udp_sock);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Set non-blocking */
    apr_socket_opt_set(udp_sock, APR_SO_NONBLOCK, 1);

    /* Bind socket */
    rv = apr_socket_bind(udp_sock, bind_addr);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "Failed to bind UDP socket to port %d", port);
        apr_socket_close(udp_sock);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Create QUIC listener data */
    ql = apr_pcalloc(pconf, sizeof(*ql));
    ql->pool = pconf;
    ql->server = s;
    ql->cert_path = cert_path;
    ql->key_path = key_path;
    ql->connections = apr_hash_make(pconf);

    /* Create SSL context */
    ql->ssl_ctx = create_quic_ssl_ctx(cert_path, key_path, s);
    if (!ql->ssl_ctx) {
        apr_socket_close(udp_sock);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Create SSL listener */
    ql->ssl_listener = SSL_new_listener(ql->ssl_ctx, 0);
    if (!ql->ssl_listener) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "Failed to create SSL listener");
        SSL_CTX_free(ql->ssl_ctx);
        apr_socket_close(udp_sock);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Attach UDP socket to SSL listener */
    if (!SSL_set_fd(ql->ssl_listener, fd)) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "Failed to attach socket to SSL listener");
        SSL_free(ql->ssl_listener);
        SSL_CTX_free(ql->ssl_ctx);
        apr_socket_close(udp_sock);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Start listening */
    if (!SSL_listen(ql->ssl_listener)) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "SSL_listen failed");
        SSL_free(ql->ssl_listener);
        SSL_CTX_free(ql->ssl_ctx);
        apr_socket_close(udp_sock);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Set non-blocking mode */
    if (!SSL_set_blocking_mode(ql->ssl_listener, 0)) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "Failed to set non-blocking mode");
        SSL_free(ql->ssl_listener);
        SSL_CTX_free(ql->ssl_ctx);
        apr_socket_close(udp_sock);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Attach QUIC listener data to socket */
    apr_socket_data_set(udp_sock, ql, "quic_listener", NULL);

    /* Create Apache listener record */
    lr = apr_pcalloc(pconf, sizeof(*lr));
    lr->sd = udp_sock;
    lr->bind_addr = bind_addr;
    lr->accept_func = ap_quic_accept;  /* <-- THE HOOK! */
    lr->flags = AP_LISTEN_REUSEPORT;
    lr->active = 1;
    lr->protocol = "h3";

    /* Add to Apache's global listener chain */
    extern ap_listen_rec *ap_listeners;
    lr->next = ap_listeners;
    ap_listeners = lr;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                 "QUIC listener registered successfully on port %d", port);

    return OK;
}

/* ====================================================================
 * Connection Creation Hook
 * ==================================================================== */

/**
 * Hook into connection creation to store QUIC state
 * Called by prefork MPM after accept returns
 */
static conn_rec *quic_create_connection(apr_pool_t *ptrans, server_rec *server,
                                         void *csd, long conn_id,
                                         void *sbh, apr_bucket_alloc_t *alloc)
{
    quic_conn_state_t *qcs = csd;

    /* Check if this is a QUIC connection (not a TCP socket) */
    if (!qcs || !qcs->ssl_stream) {
        /* Not a QUIC connection, let other modules handle it */
        return NULL;
    }

    /* Create the connection record */
    conn_rec *c = apr_pcalloc(ptrans, sizeof(conn_rec));

    c->pool = ptrans;
    c->base_server = server;
    c->local_addr = NULL;  /* TODO: get from SSL */
    c->client_addr = NULL; /* TODO: get from SSL */
    c->conn_config = ap_create_conn_config(ptrans);
    c->notes = apr_table_make(ptrans, 5);
    c->id = conn_id;
    c->bucket_alloc = alloc;

    /* Store QUIC state in connection config */
    ap_set_module_config(c->conn_config, &http3_module, qcs);

    /* Mark this as a QUIC/HTTP3 connection */
    apr_table_set(c->notes, "IS_mod_http3", "1");

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
                 "quic_create_connection: created conn_rec for stream %"
                 APR_UINT64_T_FMT, qcs->stream_id);

    return c;
}

/* ====================================================================
 * Configuration
 * ==================================================================== */

/**
 * Per-server configuration
 */
typedef struct {
    apr_uint16_t port;      /* QUIC port (0 = disabled) */
    const char *cert_file;  /* Certificate file path */
    const char *key_file;   /* Private key file path */
} quic_srv_conf;

static void *create_quic_srv_conf(apr_pool_t *p, server_rec *s)
{
    quic_srv_conf *conf = apr_pcalloc(p, sizeof(*conf));
    conf->port = 0;         /* Disabled by default */
    conf->cert_file = NULL;
    conf->key_file = NULL;
    return conf;
}

static const char *set_quic_port(cmd_parms *cmd, void *dummy, const char *arg)
{
    quic_srv_conf *conf = ap_get_module_config(cmd->server->module_config,
                                                 &http3_module);
    int port = atoi(arg);
    if (port <= 0 || port > 65535) {
        return "QUICPort must be between 1 and 65535";
    }
    conf->port = (apr_uint16_t)port;
    return NULL;
}

static const char *set_quic_cert(cmd_parms *cmd, void *dummy, const char *arg)
{
    quic_srv_conf *conf = ap_get_module_config(cmd->server->module_config,
                                                 &http3_module);
    conf->cert_file = arg;
    return NULL;
}

static const char *set_quic_key(cmd_parms *cmd, void *dummy, const char *arg)
{
    quic_srv_conf *conf = ap_get_module_config(cmd->server->module_config,
                                                 &http3_module);
    conf->key_file = arg;
    return NULL;
}

static const command_rec quic_cmds[] = {
    AP_INIT_TAKE1("QUICPort", set_quic_port, NULL, RSRC_CONF,
                  "Port number for QUIC/HTTP3 listener"),
    AP_INIT_TAKE1("QUICCertificateFile", set_quic_cert, NULL, RSRC_CONF,
                  "Path to SSL certificate file"),
    AP_INIT_TAKE1("QUICCertificateKeyFile", set_quic_key, NULL, RSRC_CONF,
                  "Path to SSL private key file"),
    { NULL }
};

/* ====================================================================
 * Module Hooks
 * ==================================================================== */

static int quic_pre_config(apr_pool_t *pconf, apr_pool_t *plog,
                            apr_pool_t *ptemp)
{
    return OK;
}

static int quic_post_config(apr_pool_t *pconf, apr_pool_t *plog,
                             apr_pool_t *ptemp, server_rec *s)
{
    quic_srv_conf *conf = ap_get_module_config(s->module_config, &http3_module);

    /* Only register listener if configured */
    if (conf->port > 0) {
        if (!conf->cert_file) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                         "QUICCertificateFile not configured but QUICPort is set");
            return HTTP_INTERNAL_SERVER_ERROR;
        }
        if (!conf->key_file) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                         "QUICCertificateKeyFile not configured but QUICPort is set");
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        return quic_register_listener(pconf, s, conf->port,
                                       conf->cert_file, conf->key_file);
    }

    return OK;
}

static void register_hooks(apr_pool_t *p)
{
    ap_hook_pre_config(quic_pre_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_config(quic_post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_create_connection(quic_create_connection, NULL, NULL, APR_HOOK_FIRST);
}

/* ====================================================================
 * Module Declaration
 * ==================================================================== */

AP_DECLARE_MODULE(http3) = {
    STANDARD20_MODULE_STUFF,
    NULL,                       /* create per-directory config structure */
    NULL,                       /* merge per-directory config structures */
    create_quic_srv_conf,       /* create per-server config structure */
    NULL,                       /* merge per-server config structures */
    quic_cmds,                  /* command table */
    register_hooks              /* register hooks */
};
