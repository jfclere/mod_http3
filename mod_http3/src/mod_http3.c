/*
 * Copyright 2023 The Apache Software Foundation.
 * Copyright (c) 2023-2026 The mod_http3 Project Authors. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * This file is derived from code originally distributed as part of
 * the Apache HTTP Server project and has been modified for use in mod_http3.
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <errno.h>
#include <httpd.h>

#include <http_config.h>
#include <http_connection.h>
#include <http_vhost.h>
#include <http_core.h>
#include <http_log.h>
#include <http_main.h>
#include <http_protocol.h>
#include <http_request.h>
#include <unistd.h>
#include <fcntl.h>
#include <util_script.h>
#ifdef HAVE_UNIX_SUEXEC
    #include <unixd.h>
#endif
#include <apr_strings.h>
#include <mpm_common.h>
#include <scoreboard.h>
#include <stdio.h>
#include <ap_listen.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "ossl-nghttp3.h"
#include "h3_session.h"

module AP_MODULE_DECLARE_DATA http3_module;

/* Server configuration structure */
typedef struct
{
    const char* cert_path;
    const char* key_path;
    apr_port_t host_port;
} h3_server_conf;

static ap_filter_rec_t* h3_net_out_filter_handle;
static ap_filter_rec_t* h3_net_in_filter_handle;
static ap_filter_rec_t* h3_proto_out_filter_handle;
static ap_filter_rec_t* h3_proto_in_filter_handle;

/* Forward declaration */
struct h3_session;

/* QUIC listener data for MPM integration */
typedef struct {
    SSL_CTX *ssl_ctx;
    SSL *ssl_listener;
    apr_pool_t *pool;
    server_rec *server;
    apr_hash_t *connections;      /* conn_id -> SSL* (QUIC connections) - parent only */
    /* Child-specific: background thread for event processing */
    apr_thread_t *event_thread;
    volatile int thread_running;
    int udp_fd;  /* UDP socket fd for thread to monitor */
    apr_socket_t *wakeup_listen_sock;  /* TCP dummy socket Apache monitors (LISTEN state) */
    apr_sockaddr_t *wakeup_connect_addr;  /* Address for thread to connect to (wake Apache) */
    /* Note: sessions hash is stored in child_sessions global, not here, due to fork() */
} h3_quic_listener_t;

/* Child-specific globals (can't store in parent structs due to fork) */
static apr_hash_t *child_sessions = NULL;
static h3_quic_listener_t *child_quic_listener = NULL;  /* This child's QUIC listener */

/* QUIC connection state */
#define H3_QUIC_MAGIC 0x48335155  /* "H3QU" in hex */
typedef struct {
    apr_uint32_t magic;         /* Magic number to identify QUIC connections */
    SSL *ssl_conn;
    SSL *ssl_stream;
    apr_uint64_t stream_id;
    apr_pool_t *pool;
    h3_quic_listener_t *listener;
    apr_socket_t *dummy_sock;   /* Dummy socket for Apache to do operations on */
} h3_quic_conn_state_t;

/* QUIC accept function for MPM integration */
/* h3_quic_accept - SIMPLE dequeue operation
 * Thread already did ALL the work (SSL_poll, accept connections, accept streams, read data)
 * Just return ready request from queue! */
/* h3_quic_accept - SIMPLE dequeue operation
 * Thread already did ALL the work (SSL_poll, accept connections, accept streams, read data)
 * Just return ready request from queue! */
static apr_status_t h3_quic_accept(void **accepted, ap_listen_rec *lr, apr_pool_t *ptrans)
{
    h3_quic_listener_t *ql;

    /* Use child-specific global */
    ql = child_quic_listener;

    if (!ql || !ql->ssl_listener) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, NULL,
                     "h3_quic_accept: invalid listener data");
        return APR_EGENERAL;
    }

    *accepted = NULL;

    /* Accept and immediately close the dummy connection that woke us up
     * This drains the wake-up signal so MPM doesn't keep calling us */
    apr_socket_t *dummy_conn = NULL;
    apr_status_t accept_rv = apr_socket_accept(&dummy_conn, ql->wakeup_listen_sock, ptrans);
    if (accept_rv == APR_SUCCESS && dummy_conn) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ql->server,
                     "h3_quic_accept: accepted and closing dummy wakeup connection");
        apr_socket_close(dummy_conn);  /* Immediately close - it was just a wakeup signal */
    }

    /* Check all sessions for ready requests (thread already filled ready_streams queue) */
    if (child_sessions && apr_hash_count(child_sessions) > 0) {
        apr_hash_index_t *hi;
        for (hi = apr_hash_first(ptrans, child_sessions); hi; hi = apr_hash_next(hi)) {
            h3_session *session = apr_hash_this_val(hi);

            /* Thread already filled ready_streams queue - just check if request is ready */
            if (session->ready_streams->nelts > 0) {
                /* We have a ready stream! Pop it from the queue */
                h3_stream **streams = (h3_stream **)session->ready_streams->elts;
                h3_stream *ready_stream = streams[0];

                /* Remove from queue (shift remaining elements) */
                if (session->ready_streams->nelts > 1) {
                    memmove(&streams[0], &streams[1],
                            (session->ready_streams->nelts - 1) * sizeof(h3_stream *));
                }
                session->ready_streams->nelts--;

                ap_log_error(APLOG_MARK, APLOG_INFO, 0, ql->server,
                             "h3_quic_accept: found ready stream %lu, creating conn_rec",
                             (unsigned long)ready_stream->stream_id);

                /* Apache is ALREADY monitoring the UDP socket in lr->sd!
                 * Just return that same socket - no need for socketpair/thread! */
                ap_listen_rec *listen_rec = NULL;
                /* Find our listener in ap_listeners */
                for (ap_listen_rec *l = ap_listeners; l; l = l->next) {
                    if (l->accept_func == h3_quic_accept) {
                        listen_rec = l;
                        break;
                    }
                }

                if (!listen_rec || !listen_rec->sd) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, ql->server,
                                 "h3_quic_accept: could not find UDP listener socket!");
                    continue;
                }

                ap_log_error(APLOG_MARK, APLOG_INFO, 0, ql->server,
                             "h3_quic_accept: returning UDP socket that Apache is already monitoring");

                apr_socket_t *udp_sock = listen_rec->sd;

                /* Create QUIC state for this stream */
                h3_quic_conn_state_t *qcs = apr_pcalloc(ptrans, sizeof(h3_quic_conn_state_t));
                qcs->magic = H3_QUIC_MAGIC;
                qcs->ssl_conn = session->ssl_conn;
                qcs->ssl_stream = ready_stream->ssl_stream;
                qcs->stream_id = ready_stream->stream_id;
                qcs->pool = ptrans;
                qcs->listener = ql;
                qcs->dummy_sock = udp_sock;  /* The UDP socket Apache is already monitoring */

                /* Attach qcs to socket */
                apr_status_t rv = apr_socket_data_set(udp_sock, qcs, "h3_state", NULL);
                if (rv != APR_SUCCESS) {
                    ap_log_error(APLOG_MARK, APLOG_WARNING, rv, ql->server,
                                 "h3_quic_accept: apr_socket_data_set failed");
                }

                /* Also store the stream pointer so we can access it later */
                rv = apr_socket_data_set(udp_sock, ready_stream, "h3_stream", NULL);
                if (rv != APR_SUCCESS) {
                    ap_log_error(APLOG_MARK, APLOG_WARNING, rv, ql->server,
                                 "h3_quic_accept: apr_socket_data_set for stream failed");
                }

                *accepted = udp_sock;
                ap_log_error(APLOG_MARK, APLOG_INFO, 0, ql->server,
                             "h3_quic_accept: returning conn_rec for stream %lu",
                             (unsigned long)ready_stream->stream_id);
                return APR_SUCCESS;
            }
        }

        /* Processed all sessions, no complete request ready */
        ap_log_error(APLOG_MARK, APLOG_TRACE8, 0, ql->server,
                     "h3_quic_accept: processed %u sessions, no request ready",
                     apr_hash_count(child_sessions));
    }
    return APR_EAGAIN;
}

/* ALPN callback for server-side QUIC */
static int h3_alpn_select_cb(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                              const unsigned char *in, unsigned int inlen, void *arg)
{
    static const unsigned char h3[] = "\x02h3";
    (void)arg;

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, NULL,
                 "h3_alpn_select_cb: client offered %u bytes of ALPN", inlen);

    /* Try to negotiate h3 from client's ALPN list */
    int result = SSL_select_next_proto((unsigned char **)out, outlen, h3, sizeof(h3) - 1,
                                        in, inlen);

    if (result == OPENSSL_NPN_NEGOTIATED) {
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, NULL,
                     "h3_alpn_select_cb: successfully negotiated h3");
        return SSL_TLSEXT_ERR_OK;
    }

    ap_log_error(APLOG_MARK, APLOG_ERR, 0, NULL,
                 "h3_alpn_select_cb: failed to negotiate h3 (result=%d)", result);
    return SSL_TLSEXT_ERR_NOACK;
}

static apr_port_t get_server_port(server_rec* s)
{
    server_addr_rec* sar;

    for (sar = s->addrs; sar; sar = sar->next)
    {
        if (sar->host_port != 0)
            return sar->host_port;
    }
    return 4433; /* XXX doc or arrange ? */
}

/* Create server configuration */
static void* h3_create_server_config(apr_pool_t* p, server_rec* /*s*/)
{
    h3_server_conf* conf = apr_pcalloc(p, sizeof(h3_server_conf));
    conf->cert_path = NULL;
    conf->key_path = NULL;
    return conf;
}

/* Merge server configuration */
static void* h3_merge_server_config(apr_pool_t* p, void* base_conf, void* new_conf)
{
    h3_server_conf* merged = apr_pcalloc(p, sizeof(h3_server_conf));
    h3_server_conf* base = (h3_server_conf*)base_conf;
    h3_server_conf* new = (h3_server_conf*)new_conf;

    merged->cert_path = new->cert_path ? new->cert_path : base->cert_path;
    merged->key_path = new->key_path ? new->key_path : base->key_path;

    return merged;
}

/* Configuration directive handlers */
static const char* set_h3_cert_path(cmd_parms* cmd, void* /*dummy*/, const char* arg)
{
    h3_server_conf* conf = ap_get_module_config(cmd->server->module_config, &http3_module);
    conf->cert_path = apr_pstrdup(cmd->pool, arg);
    return NULL;
}

static const char* set_h3_key_path(cmd_parms* cmd, void* /*dummy*/, const char* arg)
{
    h3_server_conf* conf = ap_get_module_config(cmd->server->module_config, &http3_module);
    conf->key_path = apr_pstrdup(cmd->pool, arg);
    return NULL;
}

static int h3_post_config(apr_pool_t* p, apr_pool_t* plog, apr_pool_t* ptemp, server_rec* s)
{
    h3_server_conf* conf = NULL;
    (void)plog;
    (void)ptemp;
    (void)p;

    if (ap_state_query(AP_SQ_MAIN_STATE) == AP_SQ_MS_CREATE_PRE_CONFIG)
    {
        return OK;
    }

    /* Loop for all the VirtualHost */
    server_rec* current_server = s;
    while (current_server)
    {
        conf = ap_get_module_config(current_server->module_config, &http3_module);
        if (conf->cert_path && conf->key_path)
        {
            conf->host_port = get_server_port(current_server);
            break;
        }
        current_server = current_server->next;
    }

    if (conf == NULL)
    {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "mod_http3: no server configuration found");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Check if certificate path is configured */
    if (!conf->cert_path)
    {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "mod_http3: H3CertificatePath directive is required but not configured");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Check if key path is configured */
    if (!conf->key_path)
    {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "mod_http3: H3CertificateKeyPath directive is required but not configured");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    ap_log_error(APLOG_MARK, APLOG_TRACE8, 0, s, "h3_post_config: %d cert_path=%s key_path=%s", getpid(), conf->cert_path, conf->key_path);

    /* Register a placeholder QUIC listener in parent so MPM knows about it.
     * Actual socket creation happens per-child in h3_child_init (with SO_REUSEPORT).
     * We set lr->sd = NULL here; each child will create its own socket. */

    ap_listen_rec *lr;
    apr_sockaddr_t *bind_addr;
    apr_status_t rv;

    /* Create bind address */
    rv = apr_sockaddr_info_get(&bind_addr, NULL, APR_INET6, conf->host_port, 0, p);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "mod_http3: Failed to create bind address for port %d", conf->host_port);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Create Apache listener record with NULL socket - children create real sockets */
    lr = apr_pcalloc(p, sizeof(*lr));
    lr->sd = NULL;  /* No socket in parent - children create SO_REUSEPORT sockets */
    lr->bind_addr = bind_addr;
    lr->accept_func = h3_quic_accept;
    lr->active = 1;
    lr->protocol = "h3";

    /* Add to Apache's global listener chain */
    lr->next = ap_listeners;
    ap_listeners = lr;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                 "mod_http3: Registered QUIC listener on port %d (children use SO_REUSEPORT)",
                 conf->host_port);

    return OK;
}

/* Data reader callback for nghttp3 - sends response body from h3_stream */
static nghttp3_ssize simple_read_data(nghttp3_conn *conn, int64_t stream_id,
                                      nghttp3_vec *vec, size_t veccnt,
                                      uint32_t *pflags, void *user_data,
                                      void *stream_user_data)
{
    h3_stream *stream = (h3_stream *)stream_user_data;

    if (!stream || !stream->response_data || stream->response_offset >= stream->response_len) {
        /* No body or already sent everything */
        *pflags = NGHTTP3_DATA_FLAG_EOF;
        return 0;
    }

    /* Send remaining data */
    size_t remaining = stream->response_len - stream->response_offset;
    size_t to_send = remaining < 4096 ? remaining : 4096;

    vec[0].base = (uint8_t *)&stream->response_data[stream->response_offset];
    vec[0].len = to_send;
    stream->response_offset += to_send;

    if (stream->response_offset >= stream->response_len) {
        *pflags = NGHTTP3_DATA_FLAG_EOF;
    } else {
        *pflags = NGHTTP3_DATA_FLAG_NONE;
    }

    return 1;
}

/* Forward declaration */
static int h3_hook_access_checker(request_rec* r);

/* Use the old process_request from ossl-nghttp3.c */
apr_status_t process_request(request_rec* r, h3_conn_ctx_t* h3ctx)
{
    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                 "process_request before ap_process_request: uri=%s, method=%s, method_number=%d, server=%s, r->main=%p, r->prev=%p",
                 r->uri,
                 r->method ? r->method : "(null)",
                 r->method_number,
                 r->server->server_hostname ? r->server->server_hostname : "(null)",
                 (void*)r->main,
                 (void*)r->prev);
    r->proxyreq = 0;
    r->filename = NULL;
    /* OLD CODE: Create and merge per_dir_config instead of just using lookup_defaults */
    r->per_dir_config = ap_create_per_dir_config(r->pool);
    r->per_dir_config = ap_merge_per_dir_configs(r->pool, r->server->lookup_defaults, r->per_dir_config);
    ap_set_module_config(r->request_config, &http3_module, h3ctx);
    if (!r->the_request && r->method && r->uri && r->protocol)
    {
        r->the_request = apr_psprintf(r->pool, "%s %s %s", r->method, r->uri, r->protocol);
    }
    /* OLD CODE: Just calls ap_process_request which runs hooks AND handler, then response
     * flows through output filters which capture it in h3ctx->otherpart / h3ctx->resp.
     * We DON'T call ap_invoke_handler or ap_finalize_request_protocol explicitly -
     * ap_process_request handles everything and the filters capture the response. */
    ap_process_request(r);
    ap_log_rerror(APLOG_MARK, APLOG_TRACE8, 0, r, "process_request after ap_process_request()");
    return OK;
}

/* WE DON'T NEED THAT ONE */
static int h3_hook_process_connection(conn_rec* c)
{
    const char* is_mod_http3 = apr_table_get(c->notes, "IS_mod_http3");
    ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, c, "h3_hook_process_connection %s", is_mod_http3);
    if (is_mod_http3 == NULL)
        return DECLINED;

    /* For QUIC connections, the session is already created in h3_quic_accept
     * This conn_rec represents one HTTP/3 stream (request), not the whole QUIC connection
     * Just let Apache process the request normally through the filters */
    h3_quic_conn_state_t *qcs = ap_get_module_config(c->conn_config, &http3_module);
    if (qcs && qcs->magic == H3_QUIC_MAGIC) {
        ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, c,
                      "h3_hook_process_connection: HTTP/3 stream %lu, c->master=%p, c->cs=%p",
                      (unsigned long)qcs->stream_id, (void*)c->master, (void*)c->cs);

        /* Get the h3_stream if attached */
        h3_stream *stream = (h3_stream *)apr_table_get(c->notes, "h3_stream");
        if (stream) {
            ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, c,
                          "h3_hook_process_connection: found h3_stream %lu, creating request_rec",
                          (unsigned long)stream->stream_id);

            /* CRITICAL FIX: Set c->master to non-NULL so mod_ssl's ssl_engine_status
             * returns DECLINED and skips SSL setup for HTTP/3 streams.
             * HTTP/3 streams are like HTTP/2 slave connections - they have a master
             * (the QUIC connection). We point master to itself as a simple way to
             * make it non-NULL without creating a separate master conn_rec.
             * NOTE: We don't call ap_run_pre_connection because the MPM already called it */
            c->master = c;  /* Non-NULL so mod_ssl skips us */
            c->cs = NULL;

            /* Update connection's base_server BEFORE creating request so ap_create_request uses the right server */
            if (stream->session && stream->session->s) {
                c->base_server = stream->session->s;
                ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, c,
                             "h3_hook_process_connection: updated c->base_server to %s BEFORE ap_create_request",
                             c->base_server->server_hostname ? c->base_server->server_hostname : "(null)");
            }

            /* Create request_rec using Apache's proper initialization */
            request_rec *r = ap_create_request(c);
            r->request_time = apr_time_now();
            /* Match old code exactly - assign lookup_defaults directly */
            r->per_dir_config = r->server->lookup_defaults;
            r->connection->keepalive = AP_CONN_KEEPALIVE;
            r->protocol = "HTTP/3.0";
            r->proto_num = HTTP_VERSION(3, 0);

            /* Populate from stored HTTP/3 headers */
            if (stream->method) {
                r->method = apr_pstrdup(r->pool, stream->method);
                r->method_number = ap_method_number_of(r->method);
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                             "h3_hook_process_connection: set method=%s, method_number=%d",
                             r->method, r->method_number);
            } else {
                ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                             "h3_hook_process_connection: stream->method is NULL!");
            }

            if (stream->path) {
                /* Use ap_parse_uri (Apache version) not apr_uri_parse - it does critical setup */
                ap_parse_uri(r, stream->path);
            } else {
                ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                             "h3_hook_process_connection: stream->path is NULL!");
            }

            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                         "h3_hook_process_connection: populated request - method=%s, uri=%s",
                         r->method ? r->method : "(NULL)",
                         r->uri ? r->uri : "(NULL)");
            if (stream->authority) {
                /* Put authority in Host header without port to avoid "malformed" error */
                char *authority_copy = apr_pstrdup(r->pool, stream->authority);
                char *colon = ap_strchr(authority_copy, ':');
                char *host_value = authority_copy;
                if (colon) {
                    *colon = '\0';  /* Split at colon */
                    host_value = authority_copy;  /* Points to hostname part before colon */
                    r->parsed_uri.port = atoi(colon + 1);
                    r->parsed_uri.port_str = apr_pstrdup(r->pool, colon + 1);
                }
                /* OLD CODE: Only sets Host header in r->headers_in, NOT r->hostname */
                apr_table_setn(r->headers_in, "Host", host_value);

                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                             "h3_hook_process_connection: set Host header to '%s'",
                             host_value ? host_value : "(null)");
            }
            if (stream->scheme) {
                apr_table_setn(r->headers_in, "Scheme", apr_pstrdup(r->pool, stream->scheme));
            }

            /* Copy regular headers */
            if (stream->headers) {
                apr_table_overlap(r->headers_in, stream->headers, APR_OVERLAP_TABLES_SET);
            }

            /* Set the server explicitly from the session - we already found the right one in h3_child_init
             * Don't rely on ap_update_vhost_from_headers because our server_rec->port is 0 */
            if (stream->session && stream->session->s) {
                r->server = stream->session->s;
                /* Also update the connection's base_server so Apache uses the right config for everything */
                r->connection->base_server = stream->session->s;
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                             "h3_hook_process_connection: set r->server and c->base_server to %s (from session)",
                             r->server->server_hostname ? r->server->server_hostname : "(default)");
            } else {
                /* Fallback: try vhost matching, though it will likely fail with port=0 */
                ap_update_vhost_from_headers(r);
            }

            stream->r = r;

            /* Create h3_conn_ctx_t for old code pattern */
            apr_pool_t *ctx_pool;
            apr_pool_create(&ctx_pool, r->pool);
            h3_conn_ctx_t *h3ctx = apr_pcalloc(ctx_pool, sizeof(h3_conn_ctx_t));
            h3ctx->c3reqpool = ctx_pool;
            h3ctx->s = r->server;
            h3ctx->resp = NULL;
            h3ctx->otherpart = NULL;
            h3ctx->dataheap = NULL;
            h3ctx->dataheaplen = 0;

            /* Log the request we're about to process */
            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                         "h3_hook_process_connection: processing %s %s, r->server=%s (defn:%s:%d), c->base_server=%s",
                         r->method ? r->method : "(null)",
                         r->uri ? r->uri : "(null)",
                         r->server->server_hostname ? r->server->server_hostname : "(null)",
                         r->server->defn_name ? r->server->defn_name : "?",
                         r->server->defn_line_number,
                         r->connection->base_server->server_hostname ? r->connection->base_server->server_hostname : "(null)");

            /* Process the request using the old process_request */
            process_request(r, h3ctx);

            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                         "h3_hook_process_connection: request processed, status=%d",
                         r->status);

            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                         "h3_hook_process_connection: response data - resp=%p, otherpart=%p, dataheap=%p (len=%ld)",
                         (void*)h3ctx->resp, (void*)h3ctx->otherpart,
                         (void*)h3ctx->dataheap, h3ctx->dataheaplen);

            /* Build response headers */
            nghttp3_nv nva[64];
            size_t nvlen = 0;

            /* :status pseudo-header - use h3ctx->resp if available, otherwise r->status */
            int status = h3ctx->resp ? h3ctx->resp->status : r->status;
            char *status_str = apr_psprintf(r->pool, "%d", status);
            nva[nvlen].name = (uint8_t *)":status";
            nva[nvlen].namelen = 7;
            nva[nvlen].value = (uint8_t *)status_str;
            nva[nvlen].valuelen = strlen(status_str);
            nva[nvlen].flags = NGHTTP3_NV_FLAG_NONE;
            nvlen++;

            /* Add response headers - prefer h3ctx->resp, fallback to r->headers_out */
            apr_table_t *headers = h3ctx->resp && h3ctx->resp->headers ? h3ctx->resp->headers : r->headers_out;
            if (headers) {
                const apr_array_header_t *tarr = apr_table_elts(headers);
                const apr_table_entry_t *telts = (const apr_table_entry_t*)tarr->elts;
                for (int i = 0; i < tarr->nelts && nvlen < 63; i++) {
                    if (telts[i].key) {
                        nva[nvlen].name = (uint8_t *)telts[i].key;
                        nva[nvlen].namelen = strlen(telts[i].key);
                        nva[nvlen].value = (uint8_t *)(telts[i].val ? telts[i].val : "");
                        nva[nvlen].valuelen = telts[i].val ? strlen(telts[i].val) : 0;
                        nva[nvlen].flags = NGHTTP3_NV_FLAG_NONE;
                        nvlen++;
                    }
                }
            }

            /* Store response body in stream for data reader callback */
            stream->response_data = NULL;
            stream->response_len = 0;
            stream->response_offset = 0;

            /* Read response body - following the pattern from ossl-nghttp3.c process_h3response() */
            if (h3ctx->otherpart != NULL) {
                apr_bucket *b = h3ctx->otherpart;
                uint8_t *buffer = NULL;
                apr_size_t len = 0;

                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                             "h3_hook_process_connection: otherpart bucket type=%s", b->type->name);

                if (APR_BUCKET_IS_FILE(b)) {
                    /* FILE bucket - read using apr_file functions like old code does */
                    apr_bucket_file *f = (apr_bucket_file *)b->data;
                    apr_file_t *fd = f->fd;
                    apr_off_t offset = b->start;
                    apr_status_t rv;

                    len = b->length;
                    rv = apr_file_seek(fd, APR_SET, &offset);
                    if (rv != APR_SUCCESS) {
                        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                                     "h3_hook_process_connection: apr_file_seek failed: %d", rv);
                    } else {
                        buffer = apr_palloc(r->pool, len);
                        rv = apr_file_read(fd, buffer, &len);
                        if (rv != APR_SUCCESS && rv != APR_EOF) {
                            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                                         "h3_hook_process_connection: apr_file_read failed: %d", rv);
                            buffer = NULL;
                            len = 0;
                        } else {
                            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                                         "h3_hook_process_connection: read FILE bucket, %ld bytes", len);
                        }
                    }
                } else if (APR_BUCKET_IS_MMAP(b) || APR_BUCKET_IS_HEAP(b)) {
                    /* MMAP or HEAP bucket - use apr_bucket_read */
                    const char *data = NULL;
                    apr_status_t rv;

                    len = b->length;
                    rv = apr_bucket_read(b, &data, &len, APR_BLOCK_READ);
                    if (rv != APR_SUCCESS || data == NULL) {
                        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                                     "h3_hook_process_connection: apr_bucket_read failed: %d", rv);
                    } else if (len > 0) {
                        buffer = apr_palloc(r->pool, len);
                        memcpy(buffer, data, len);
                        ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                                     "h3_hook_process_connection: read %s bucket, %ld bytes",
                                     b->type->name, len);
                    }
                } else {
                    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                                 "h3_hook_process_connection: unsupported bucket type %s", b->type->name);
                }

                if (buffer && len > 0) {
                    stream->response_data = buffer;
                    stream->response_len = len;
                }
            } else if (h3ctx->dataheap) {
                /* Error page or heap data */
                stream->response_data = (const uint8_t *)h3ctx->dataheap;
                stream->response_len = h3ctx->dataheaplen;
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                             "h3_hook_process_connection: sending HEAP body, %ld bytes", stream->response_len);
            } else {
                /* No body */
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                             "h3_hook_process_connection: no body to send");
            }

            /* Submit response with body */
            nghttp3_data_reader dr;
            dr.read_data = simple_read_data;

            int rv = nghttp3_conn_submit_response(stream->session->ngh3, stream->stream_id,
                                                  nva, nvlen, stream->response_len > 0 ? &dr : NULL);
            if (rv != 0) {
                ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                             "nghttp3_conn_submit_response failed: %d", rv);
            } else {
                ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                             "nghttp3_conn_submit_response succeeded with %ld headers, body=%ld bytes",
                             nvlen, stream->response_len);
            }

            /* Thread will flush the response - we just queue it in nghttp3 */
            ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
                         "h3_hook_process_connection: response queued, thread will send it");

            return OK;
        }

        /* No stream attached - shouldn't happen */
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, c,
                     "h3_hook_process_connection: no stream attached!");
        return DECLINED;
    }

    /* Not a QUIC connection, continue with normal processing */
    return DECLINED;
}

static int h3_hook_pre_connection(conn_rec* c, void* /*csd*/)
{
    const char* is_mod_http3 = apr_table_get(c->notes, "IS_mod_http3");
    ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, c, "h3_hook_pre_connection %s", is_mod_http3);
    if (is_mod_http3 == NULL)
        return DECLINED;

    /* CRITICAL FIX: Tell mod_ssl to bypass SSL setup for HTTP/3 connections.
     * HTTP/3 uses QUIC which handles TLS at the QUIC connection level,
     * so we don't need mod_ssl's SSL handling on HTTP/3 streams.
     * Setting "ssl-bypass" note prevents mod_ssl from:
     * 1. Creating SSLConnRec and adding SSL filters
     * 2. Returning HTTP_FORBIDDEN (403) from ssl_hook_Access
     * This is the standard way to tell mod_ssl to skip a connection. */
    apr_table_setn(c->notes, "ssl-bypass", "1");
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c,
                  "h3_hook_pre_connection: QUIC connection, set ssl-bypass note");
    return OK;
}

static int h3_hook_post_read_request(request_rec* r)
{
    ap_log_rerror(APLOG_MARK, APLOG_TRACE8, 0, r, "h3_hook_ap_hook_post_read_request");
    return OK;
}
static void h3_hook_pre_read_request(request_rec* r, conn_rec* /*c*/)
{
    ap_log_rerror(APLOG_MARK, APLOG_TRACE8, 0, r, "h3_hook_ap_hook_pre_read_request");
}

static int h3_hook_access_checker(request_rec* r)
{
    /* CRITICAL DEBUG: Let's see EXACTLY what's happening */
    static int call_count = 0;
    call_count++;

    const char* is_http3 = apr_table_get(r->connection->notes, "IS_mod_http3");

    int retval = OK;  /* OK is defined as 0 in httpd.h */
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                 "h3_hook_access_checker CALL #%d: uri=%s, is_http3=%s, returning %d (OK=%d)",
                 call_count, r->uri, is_http3 ? is_http3 : "NULL", retval, OK);
    return retval;
}

static int h3_hook_access_checker2(request_rec* r)
{
    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                 "h3_hook_access_checker2: This should NEVER be called! Returning DECLINED");
    return DECLINED;
}

static apr_status_t h3_filter_out(ap_filter_t* f, apr_bucket_brigade* bb)
{
    apr_bucket* b;
    h3_quic_conn_state_t *qcs;

    ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, f->c, "h3_filter_out CALLED");

    /* Check if this is a QUIC connection */
    qcs = ap_get_module_config(f->c->conn_config, &http3_module);

    if (!qcs || qcs->magic != H3_QUIC_MAGIC) {
        /* Not a QUIC connection - pass through */
        ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out: not QUIC, passing through");
        ap_remove_output_filter(f);
        return ap_pass_brigade(f->next, bb);
    }

    /* QUIC connection - write to SSL stream */
    for (b = APR_BRIGADE_FIRST(bb); b != APR_BRIGADE_SENTINEL(bb); b = APR_BUCKET_NEXT(b))
    {
        if (APR_BUCKET_IS_METADATA(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out APR_BUCKET_IS_METADATA");
        }
        if (APR_BUCKET_IS_FLUSH(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out APR_BUCKET_IS_FLUSH");
        }
        if (APR_BUCKET_IS_EOS(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out APR_BUCKET_IS_EOS");
            /* Don't send FIN here - nghttp3 will send FIN when response is complete
             * and h3_session will call SSL_stream_conclude() after writing */
        }
        if (AP_BUCKET_IS_ERROR(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out AP_BUCKET_IS_ERROR");
        }
        if (AP_BUCKET_IS_EOC(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out AP_BUCKET_IS_EOC");
        }
        if (APR_BUCKET_IS_FILE(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out APR_BUCKET_IS_FILE");
        }
        if (AP_BUCKET_IS_HEADERS(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out AP_BUCKET_IS_HEADERS");
        }
        if (APR_BUCKET_IS_FLUSH(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out APR_BUCKET_IS_FLUSH");
        }
        if (APR_BUCKET_IS_IMMORTAL(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out APR_BUCKET_IS_IMMORTAL");
        }
        if (APR_BUCKET_IS_HEAP(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out APR_BUCKET_IS_HEAP");
        }
        if (APR_BUCKET_IS_MMAP(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out APR_BUCKET_IS_MMAP");
        }
        if (AP_BUCKET_IS_EOR(b))
        {
            /* the response/request done */
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out AP_BUCKET_IS_EOR");
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out DONE");
            return DONE;
        }
        if (AP_BUCKET_IS_RESPONSE(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out AP_BUCKET_IS_RESPONSE");
        }

        /* Write data buckets to QUIC stream */
        if (!APR_BUCKET_IS_METADATA(b)) {
            const char *data;
            apr_size_t len;
            apr_status_t rv;

            rv = apr_bucket_read(b, &data, &len, APR_BLOCK_READ);
            if (rv == APR_SUCCESS && len > 0) {
                size_t written = 0;
                int ssl_ret = SSL_write_ex(qcs->ssl_stream, data, len, &written);

                if (ssl_ret > 0) {
                    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, f->c,
                                 "h3_filter_out: wrote %ld bytes to QUIC stream", written);
                } else {
                    int ssl_err = SSL_get_error(qcs->ssl_stream, ssl_ret);
                    ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, f->c,
                                 "h3_filter_out: SSL_write_ex failed (ssl_err=%d)", ssl_err);
                    return APR_EGENERAL;
                }
            }
        }
    }

    ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out DONE");
    return APR_SUCCESS;
}

static int print_table_entry(void* rec, const char* key, const char* value)
{
    const conn_rec* c = (conn_rec*)rec;
    ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, c, "h3_filter_out_proto print_table_entry %s %s", key, value);
    return 1;
}

static apr_status_t h3_filter_out_proto(ap_filter_t* f, apr_bucket_brigade* bb)
{
    apr_bucket* b = NULL;
    apr_status_t rv = 0;
    h3_conn_ctx_t* ctx = (h3_conn_ctx_t*)ap_get_module_config(f->r->request_config, &http3_module);
    ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto %p START", (void*)ctx);
    if (ctx == NULL)
        return ap_pass_brigade(f->next, bb);

    ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto %d", f->r->status);

    for (b = APR_BRIGADE_FIRST(bb); b != APR_BRIGADE_SENTINEL(bb); b = APR_BUCKET_NEXT(b))
    {
        if (APR_BUCKET_IS_METADATA(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto APR_BUCKET_IS_METADATA");
        }
        if (APR_BUCKET_IS_FLUSH(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto APR_BUCKET_IS_FLUSH");
        }
        else if (APR_BUCKET_IS_EOS(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto APR_BUCKET_IS_EOS");
        }
        else if (AP_BUCKET_IS_ERROR(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto AP_BUCKET_IS_ERROR");
        }
        else if (AP_BUCKET_IS_EOC(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto AP_BUCKET_IS_EOC");
        }
        else if (APR_BUCKET_IS_FILE(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto APR_BUCKET_IS_FILE");
        }
        else if (AP_BUCKET_IS_HEADERS(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto AP_BUCKET_IS_HEADERS");
        }
        else if (APR_BUCKET_IS_HEAP(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto APR_BUCKET_IS_HEAP");
        }
        else if (AP_BUCKET_IS_EOR(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto AP_BUCKET_IS_EOR");
        }
        else if (AP_BUCKET_IS_RESPONSE(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto AP_BUCKET_IS_RESPONSE");
        }
        else
        {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto AP_BUCKET_IS_SOMETHING %s", b->type->name);
        }

        if (AP_BUCKET_IS_ERROR(b))
        {
            /* Should we generate the error page here */
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto AP_BUCKET_IS_ERROR");
            ap_send_error_response(f->r, 0);
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto AP_BUCKET_IS_ERROR after ap_send_error_response()");
            return OK;
        }
        if (APR_BUCKET_IS_FILE(b) || APR_BUCKET_IS_MMAP(b) || APR_BUCKET_IS_HEAP(b))
        {
            ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, f->c, "h3_filter_out_proto found %s bucket (len=%ld) - reading NOW",
                         b->type->name, (size_t)b->length);
            /* Read bucket data NOW while file is still open */
            const char *data = NULL;
            apr_size_t len = 0;
            ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, f->c, "h3_filter_out_proto calling apr_bucket_read...");
            apr_status_t rv = apr_bucket_read(b, &data, &len, APR_BLOCK_READ);
            ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, f->c, "h3_filter_out_proto apr_bucket_read returned: rv=%d, len=%ld, data=%p",
                         rv, len, (void*)data);
            if (rv == APR_SUCCESS && len > 0 && data != NULL) {
                /* Copy to ctx pool which is tied to connection, not request */
                ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, f->c, "h3_filter_out_proto copying data to ctx->dataheap...");
                char *data_copy = apr_pmemdup(ctx->c3reqpool, data, len);
                ctx->dataheap = data_copy;
                ctx->dataheaplen = len;
                ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, f->c,
                             "h3_filter_out_proto: copied %ld bytes from %s bucket to ctx",
                             len, b->type->name);
            } else {
                ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, f->c,
                             "h3_filter_out_proto: failed to read %s bucket: rv=%d, len=%ld",
                             b->type->name, rv, len);
            }
        }
        else if (0)  /* DISABLED old code */
        {
            if (!f->r || !f->r->request_config) {
                ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, f->c,
                             "h3_filter_out_proto: f->r or request_config is NULL, skipping");
                APR_BUCKET_REMOVE(b);
                continue;
            }
            h3_conn_ctx_t* inner_ctx = (h3_conn_ctx_t*)ap_get_module_config(f->r->request_config, &http3_module);
            if (inner_ctx != NULL)
            {
                /* OLD CODE - disabled */
            }
            else
            {
                ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto body bucket NO CTX");
            }
        }
        if (AP_BUCKET_IS_RESPONSE(b))
        {
            ap_bucket_response* resp = b->data;
            h3_conn_ctx_t* inner_ctx = (h3_conn_ctx_t*)ap_get_module_config(f->r->request_config, &http3_module);
            /* we will process the response information */
            APR_BUCKET_REMOVE(b);
            apr_bucket_setaside(b, inner_ctx->c3reqpool);
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto AP_BUCKET_IS_RESPONSE");
            if (inner_ctx != NULL)
            {
                inner_ctx->resp = resp;
                if (inner_ctx->otherpart != NULL)
                {
                    ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto AP_BUCKET_IS_RESPONSE otherpart %s", inner_ctx->otherpart->type->name);
                }
            }
            else
            {
                ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto AP_BUCKET_IS_RESPONSE NO CTX!!!!");
            }
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto status: %d", resp->status);
            /* XXX: just debug information */
            if (resp->reason != NULL)
            {
                ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto reason: %s", resp->reason);
            }
            if (resp->headers != NULL)
            {
                apr_table_do(print_table_entry, (void*)f->c, resp->headers, NULL);
            }
            if (resp->notes != NULL)
            {
                apr_table_do(print_table_entry, (void*)f->c, resp->notes, NULL);
            }
        }
        if (APR_BUCKET_IS_HEAP(b))
        {
            h3_conn_ctx_t* inner_ctx = (h3_conn_ctx_t*)ap_get_module_config(f->r->request_config, &http3_module);
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto APR_BUCKET_IS_HEAP");
            if (inner_ctx != NULL && b->data != NULL)
            {
                const char* data;
                apr_size_t len;
                /* We will process it. */
                APR_BUCKET_REMOVE(b);
                apr_bucket_setaside(b, inner_ctx->c3reqpool);
                apr_bucket_read(b, &data, &len, APR_BLOCK_READ);
                inner_ctx->dataheap = (char*)data;
                inner_ctx->dataheaplen = len;
            }
            else
            {
                ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, f->c, "h3_filter_out_proto APR_BUCKET_IS_HEAP NO CTX or NO DATA!!!!");
            }
        }
        if (AP_BUCKET_IS_EOR(b))
        {
            /* EOR belongs to network filters! */
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto AP_BUCKET_IS_EOR");
        }
    }
    if (ctx != NULL && ctx->otherpart != NULL && ctx->resp != NULL)
    {
        /* we are done, just return */
        ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto %d %d %p DONE", rv, f->r->status, (void*)f->r->connection);
        return OK;
    }
    ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto CALLING ap_pass_brigade() on next");
    rv = ap_pass_brigade(f->next, bb);
    ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_out_proto %d %d %p DONE", rv, f->r->status, (void*)f->r->connection);

    return rv;
}

static apr_status_t h3_filter_in_proto(ap_filter_t* f, apr_bucket_brigade* bb, ap_input_mode_t mode, apr_read_type_e block, apr_off_t readbytes)
{
    apr_status_t rv;
    h3_conn_ctx_t* ctx = (h3_conn_ctx_t*)ap_get_module_config(f->r->request_config, &http3_module);
    ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_in_proto %p", (void*)ctx);
    if (ctx == NULL)
        return ap_get_brigade(f->next, bb, mode, block, readbytes);

    if (mode != AP_MODE_READBYTES && mode != AP_MODE_GETLINE)
    {
        ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_in_proto let's do nothing!");
        return ap_get_brigade(f->next, bb, mode, block, readbytes);
    }
    ap_remove_input_filter(f);
    if (mode == AP_MODE_READBYTES)
    {
        ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_in_proto AP_MODE_READBYTES status %d %ld", f->r->status, (long)f->r->clength);
        if (APR_BRIGADE_EMPTY(bb))
        {
            const char* postdata = apr_table_get(f->r->notes, "H3POSTDATA");
            const char* postdatalen = apr_table_get(f->r->notes, "H3POSTDATALEN");
            if (postdatalen && postdata)
            {
                apr_int64_t data_len64 = apr_atoi64(postdatalen);
                if (data_len64 < 0 || (uint64_t)data_len64 > (uint64_t)APR_SIZE_MAX)
                {
                    ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, f->c, "h3_filter_in_proto: invalid data length");
                    return APR_EGENERAL;
                }
                apr_size_t data_len = (apr_size_t)data_len64;
                apr_status_t write_rv = apr_brigade_write(bb, NULL, NULL, postdata, data_len);
                if (write_rv != APR_SUCCESS)
                {
                    ap_log_cerror(APLOG_MARK, APLOG_ERR, write_rv, f->c, "h3_filter_in_proto: brigade write failed");
                    return write_rv;
                }
                f->r->clength = (apr_off_t)data_len;
            }
            ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_in_proto AP_MODE_READBYTES add EOS");
            apr_bucket* eos;
            eos = apr_bucket_eos_create(f->c->bucket_alloc);
            APR_BRIGADE_INSERT_TAIL(bb, eos);
        }
        return APR_SUCCESS;
    }
    ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_in_proto OTHER status %d", f->r->status);
    rv = ap_pass_brigade(f->next, bb);
    ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_in_proto %d %d %d", rv, mode, AP_MODE_READBYTES);
    return APR_SUCCESS;
}

static apr_status_t h3_filter_in(ap_filter_t* f, apr_bucket_brigade* bb, ap_input_mode_t mode, apr_read_type_e block, apr_off_t readbytes)
{
    h3_quic_conn_state_t *qcs;

    ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, f->c, "h3_filter_in mode %d CALLED", mode);

    /* Check if this is a QUIC connection */
    qcs = ap_get_module_config(f->c->conn_config, &http3_module);

    if (!qcs || qcs->magic != H3_QUIC_MAGIC) {
        /* Not a QUIC connection - pass through */
        ap_log_cerror(APLOG_MARK, APLOG_TRACE8, 0, f->c, "h3_filter_in: not QUIC, removing filter");
        ap_remove_input_filter(f);
        return ap_get_brigade(f->next, bb, mode, block, readbytes);
    }

    /* QUIC connection - synthesize HTTP/1.1 request from HTTP/3 headers
     * We already parsed the HTTP/3 frames via nghttp3, so we can't read them again.
     * Instead, provide a fake HTTP/1.1 request for Apache's protocol handler.
     * TODO: Store headers in h3_stream and build proper request here */

    /* Return EOS bucket to signal no request body (GET request)
     * For filters, APR_SUCCESS means "I successfully provided what you asked for (which is EOS)" */
    apr_bucket *e = apr_bucket_eos_create(f->c->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(bb, e);

    ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, f->c,
                 "h3_filter_in: returning EOS bucket with APR_SUCCESS (no request body for GET)");

    return APR_SUCCESS;
}

/* Background thread - the COMPLETE QUIC event processor
 * Does ALL QUIC work: accept connections, create streams, read data, parse HTTP/3
 * Wakes MPM ONLY when a complete HTTP request is ready */
static void* APR_THREAD_FUNC quic_event_thread(apr_thread_t *thread, void *data)
{
    (void)thread;
    h3_quic_listener_t *ql = (h3_quic_listener_t*)data;
    fd_set read_fd, write_fd;
    struct timeval tv, *tvp;
    int isinfinite;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, ql->server,
                 "quic_event_thread: started in PID %d - COMPLETE QUIC EVENT PROCESSOR",
                 getpid());

    while (ql->thread_running) {
        /* wait_for_activity() - select on UDP socket + timeout */
        FD_ZERO(&read_fd);
        FD_ZERO(&write_fd);

        if (SSL_net_write_desired(ql->ssl_listener))
            FD_SET(ql->udp_fd, &write_fd);
        if (SSL_net_read_desired(ql->ssl_listener))
            FD_SET(ql->udp_fd, &read_fd);
        FD_SET(ql->udp_fd, &read_fd); /* Always monitor for read */

        tvp = NULL;
        if (SSL_get_event_timeout(ql->ssl_listener, &tv, &isinfinite) && !isinfinite) {
            if (tv.tv_sec != 0 || tv.tv_usec != 0)
                tvp = &tv;
        }

        /* Always use a reasonable timeout to avoid hanging forever */
        struct timeval max_timeout = {1, 0};  /* 1 second */
        if (!tvp || (tv.tv_sec > 1)) {
            tvp = &max_timeout;
        }

        int ret = select(ql->udp_fd + 1, &read_fd, &write_fd, NULL, tvp);
        if (ret < 0) {
            if (errno == EINTR) continue;
            ap_log_error(APLOG_MARK, APLOG_ERR, errno, ql->server,
                         "quic_event_thread: select failed");
            break;
        }

        if (ret > 0) {
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, ql->server,
                         "quic_event_thread: select returned %d - UDP socket has activity", ret);
        }

        /* Process internal QUIC events */
        SSL_handle_events(ql->ssl_listener);

        /* Call SSL_poll ONCE per socket wakeup - NOT in a loop! */
        int has_ready_request = 0;

        /* Build poll array: listener + all connections + all streams */
        SSL_POLL_ITEM poll_items[256];
        int num_items = 0;

            poll_items[num_items].desc = SSL_as_poll_descriptor(ql->ssl_listener);
            poll_items[num_items].events = UINT64_MAX;
            poll_items[num_items].revents = 0;
            num_items++;

            /* Add all sessions - both connections AND streams */
            if (child_sessions) {
                apr_hash_index_t *hi;
                for (hi = apr_hash_first(NULL, child_sessions); hi && num_items < 256; hi = apr_hash_next(hi)) {
                    h3_session *session = apr_hash_this_val(hi);
                    if (session && session->ssl_conn) {
                        /* Add the CONNECTION to poll for IC/OSU/ISB/ISU events */
                        poll_items[num_items].desc = SSL_as_poll_descriptor(session->ssl_conn);

                        /* Poll for all events, but EXCLUDE OSU if control streams already created
                         * OSU is level-triggered and stays true forever after handshake completes */
                        if (session->control_streams_created) {
                            /* Control streams done - only poll for stream events (ISB/ISU/R/W), not OSU */
                            poll_items[num_items].events = SSL_POLL_EVENT_ISB | SSL_POLL_EVENT_ISU |
                                                           SSL_POLL_EVENT_R | SSL_POLL_EVENT_W;
                        } else {
                            /* Control streams not created yet - poll for OSU + stream events */
                            poll_items[num_items].events = UINT64_MAX;
                        }

                        poll_items[num_items].revents = 0;
                        num_items++;

                        /* Add ALL streams from this session to the poll array for R/W events */
                        if (session->streams && num_items < 256) {
                            apr_hash_index_t *stream_hi;
                            for (stream_hi = apr_hash_first(NULL, session->streams); stream_hi && num_items < 256; stream_hi = apr_hash_next(stream_hi)) {
                                h3_stream *h3s = apr_hash_this_val(stream_hi);
                                if (h3s && h3s->ssl_stream) {
                                    poll_items[num_items].desc = SSL_as_poll_descriptor(h3s->ssl_stream);
                                    /* Poll for R (readable) always, W (writable) only if we have data to send */
                                    poll_items[num_items].events = SSL_POLL_EVENT_R;
                                    poll_items[num_items].revents = 0;
                                    num_items++;
                                }
                            }
                        }
                    }
                }
            }

        static const struct timeval zero_timeout = {0, 0};
        size_t result_count = 0;
        int poll_ret = SSL_poll(poll_items, num_items, sizeof(SSL_POLL_ITEM), &zero_timeout,
                               SSL_POLL_FLAG_NO_HANDLE_EVENTS, &result_count);

        ap_log_error(APLOG_MARK, APLOG_INFO, 0, ql->server,
                     "quic_event_thread: SSL_poll returned %d, result_count=%lu, num_items=%d",
                     poll_ret, (unsigned long)result_count, num_items);

        /* Process events only if there are any */
        if (poll_ret && result_count > 0) {

            /* Count events we process to ensure we handle ALL of them */
            size_t events_processed = 0;

            /* Process ALL events */
            for (int i = 0; i < num_items; i++) {
                if (poll_items[i].revents == SSL_POLL_EVENT_NONE)
                    continue;

                events_processed++;

                ap_log_error(APLOG_MARK, APLOG_INFO, 0, ql->server,
                             "quic_event_thread: poll_items[%d].revents = 0x%lx (IC=%d OSU=%d ISB=%d ISU=%d R=%d W=%d)",
                             i, (unsigned long)poll_items[i].revents,
                             !!(poll_items[i].revents & SSL_POLL_EVENT_IC),
                             !!(poll_items[i].revents & SSL_POLL_EVENT_OSU),
                             !!(poll_items[i].revents & SSL_POLL_EVENT_ISB),
                             !!(poll_items[i].revents & SSL_POLL_EVENT_ISU),
                             !!(poll_items[i].revents & SSL_POLL_EVENT_R),
                             !!(poll_items[i].revents & SSL_POLL_EVENT_W));

                /* IC on listener - accept new connection */
                if (i == 0 && (poll_items[i].revents & SSL_POLL_EVENT_IC)) {
                    ap_log_error(APLOG_MARK, APLOG_INFO, 0, ql->server,
                                 "quic_event_thread: IC event - accepting connection");
                    SSL *conn = SSL_accept_connection(ql->ssl_listener, SSL_ACCEPT_STREAM_NO_BLOCK);
                    if (conn) {
                        SSL_set_blocking_mode(conn, 0);
                        SSL_set_default_stream_mode(conn, SSL_DEFAULT_STREAM_MODE_NONE);

                        h3_session *session = NULL;
                        apr_status_t rv = h3_session_create(&session, ql->server, ql->ssl_listener, conn, ql->pool);
                        if (rv == APR_SUCCESS && child_sessions) {
                            apr_uint64_t conn_id = (apr_uint64_t)(uintptr_t)conn;
                            apr_uint64_t *conn_key = apr_pmemdup(ql->pool, &conn_id, sizeof(conn_id));
                            apr_hash_set(child_sessions, conn_key, sizeof(*conn_key), session);
                        }
                    }
                    continue;
                }

                /* Events on connections or streams (i > 0) */
                if (i > 0 && child_sessions) {
                    int handled = 0;

                    /* Check all sessions */
                    apr_hash_index_t *hi;
                    for (hi = apr_hash_first(NULL, child_sessions); hi; hi = apr_hash_next(hi)) {
                        h3_session *session = apr_hash_this_val(hi);
                        if (!session) continue;

                        /* Is this event on the CONNECTION? */
                        if (session->ssl_conn == poll_items[i].desc.value.ssl) {
                            /* OSU - create control streams ONCE */
                            if ((poll_items[i].revents & SSL_POLL_EVENT_OSU) && !session->control_streams_created) {
                                h3_session_create_control_streams(session);
                            }

                            /* Process session ONLY if there are stream events (ISB/ISU/R/W), NOT for OSU alone */
                            uint64_t stream_events = poll_items[i].revents & (SSL_POLL_EVENT_ISB | SSL_POLL_EVENT_ISU | SSL_POLL_EVENT_R | SSL_POLL_EVENT_W);
                            if (stream_events) {
                                h3_session_process(session, NULL);  /* NULL = accept NEW streams */
                            }

                            /* Check if request is ready */
                            if (session->ready_streams->nelts > 0) {
                                has_ready_request = 1;
                            }
                            handled = 1;
                            break;
                        }

                        /* Is this event on a STREAM? */
                        if (session->streams && !handled) {
                            apr_hash_index_t *stream_hi;
                            for (stream_hi = apr_hash_first(NULL, session->streams); stream_hi; stream_hi = apr_hash_next(stream_hi)) {
                                h3_stream *h3s = apr_hash_this_val(stream_hi);
                                if (h3s && h3s->ssl_stream == poll_items[i].desc.value.ssl) {
                                    /* R/W event on THIS specific stream - read from it! */
                                    if (poll_items[i].revents & (SSL_POLL_EVENT_R | SSL_POLL_EVENT_W)) {
                                        h3_session_process(session, h3s);  /* Pass the SPECIFIC stream */

                                        /* Check if request is ready */
                                        if (session->ready_streams->nelts > 0) {
                                            has_ready_request = 1;
                                        }
                                    }
                                    handled = 1;
                                    break;
                                }
                            }
                            if (handled) break;
                        }
                    }
                }
            }

            /* Verify we processed ALL events that SSL_poll reported */
            if (events_processed != result_count) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, ql->server,
                             "quic_event_thread: BUG - SSL_poll reported %lu events but we only processed %lu - ABORTING",
                             (unsigned long)result_count, (unsigned long)events_processed);
                abort();
            }
        } /* end if (poll_ret && result_count > 0) */

        /* Flush nghttp3 output - send any queued response data */
        if (child_sessions && apr_hash_count(child_sessions) > 0) {
            apr_hash_index_t *flush_hi;
            for (flush_hi = apr_hash_first(NULL, child_sessions); flush_hi; flush_hi = apr_hash_next(flush_hi)) {
                h3_session *flush_session = apr_hash_this_val(flush_hi);
                if (!flush_session || !flush_session->ngh3) continue;

                /* Ask nghttp3 which streams have data to send */
                for (;;) {
                    nghttp3_vec vec[16];
                    nghttp3_ssize nvec;
                    int64_t stream_id;
                    int fin = 0;

                    /* nghttp3 tells US which stream has data */
                    nvec = nghttp3_conn_writev_stream(flush_session->ngh3, &stream_id,
                                                      &fin, vec, 16);
                    if (nvec == 0) {
                        /* No more data to send */
                        break;
                    }
                    if (nvec < 0) {
                        ap_log_error(APLOG_MARK, APLOG_ERR, 0, ql->server,
                                     "quic_event_thread: nghttp3_conn_writev_stream failed: %ld", (long)nvec);
                        break;
                    }

                    /* Find the h3_stream for this stream_id */
                    h3_stream *h3s = apr_hash_get(flush_session->streams, &stream_id, sizeof(stream_id));
                    if (!h3s || !h3s->ssl_stream) {
                        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, ql->server,
                                     "quic_event_thread: stream %ld not found in hash", (long)stream_id);
                        continue;
                    }

                    /* Send all vectors */
                    size_t total_written = 0;
                    for (nghttp3_ssize i = 0; i < nvec; i++) {
                        size_t written = 0;
                        int write_ret = SSL_write_ex(h3s->ssl_stream, vec[i].base, vec[i].len, &written);
                        if (write_ret > 0) {
                            total_written += written;
                        } else {
                            int ssl_err = SSL_get_error(h3s->ssl_stream, write_ret);
                            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, ql->server,
                                         "quic_event_thread: SSL_write_ex failed on stream %ld: ssl_err=%d",
                                         (long)stream_id, ssl_err);
                            break;
                        }
                    }

                    if (total_written > 0) {
                        ap_log_error(APLOG_MARK, APLOG_INFO, 0, ql->server,
                                     "quic_event_thread: wrote %zu bytes to stream %ld (%ld vectors, fin=%d)",
                                     total_written, (long)stream_id, (long)nvec, fin);

                        /* Tell nghttp3 we consumed the data */
                        int consumed = nghttp3_conn_add_write_offset(flush_session->ngh3, stream_id, total_written);
                        if (consumed != 0) {
                            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, ql->server,
                                         "quic_event_thread: nghttp3_conn_add_write_offset failed: %d", consumed);
                        }
                    }

                    /* If fin, close the stream write side */
                    if (fin) {
                        SSL_stream_conclude(h3s->ssl_stream, 0);
                        ap_log_error(APLOG_MARK, APLOG_INFO, 0, ql->server,
                                     "quic_event_thread: closed write side of stream %ld (FIN)",
                                     (long)stream_id);
                    }
                }
            }
        }

        /* Wake MPM ONLY if we have a ready request */
        if (has_ready_request) {
            /* Connect to dummy socket to wake Apache - MPM will call h3_quic_accept */
            int wake_sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            if (wake_sock >= 0) {
                struct sockaddr_in wake_addr;
                memset(&wake_addr, 0, sizeof(wake_addr));
                wake_addr.sin_family = AF_INET;
                wake_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                wake_addr.sin_port = htons(ql->wakeup_connect_addr->port);

                /* Non-blocking connect - we don't care if it succeeds or EINPROGRESS */
                int conn_ret = connect(wake_sock, (struct sockaddr*)&wake_addr, sizeof(wake_addr));
                if (conn_ret == 0 || errno == EINPROGRESS) {
                    ap_log_error(APLOG_MARK, APLOG_INFO, 0, ql->server,
                                 "quic_event_thread: REQUEST READY - connected to dummy socket port %d",
                                 ql->wakeup_connect_addr->port);
                } else {
                    ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ql->server,
                                 "quic_event_thread: failed to connect to dummy socket");
                }
                /* Don't close yet - keep connection alive so MPM sees it */
                /* Socket will be cleaned up by h3_quic_accept */
            } else {
                ap_log_error(APLOG_MARK, APLOG_ERR, errno, ql->server,
                             "quic_event_thread: failed to create wakeup socket");
            }
        }
    }

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, ql->server,
                 "quic_event_thread: exiting in PID %d", getpid());
    return NULL;
}

/* Child initialization: create per-child QUIC listener */
static void h3_child_init(apr_pool_t* pchild, server_rec* s)
{
    h3_quic_listener_t *ql;
    apr_socket_t *udp_sock;
    apr_sockaddr_t *bind_addr;
    ap_listen_rec *lr;
    apr_status_t rv;
    apr_os_sock_t fd;
    BIO *bio;
    h3_server_conf* conf;

    /* Create child-specific sessions hash (can't modify parent's ql after fork) */
    child_sessions = apr_hash_make(pchild);

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                 "h3_child_init: created child-specific sessions hash");

    /* Find the server config with cert/key configured - search ALL servers and log them */
    server_rec* current_server = s;
    conf = NULL;
    while (current_server) {
        h3_server_conf* tmp_conf = ap_get_module_config(current_server->module_config, &http3_module);
        int port = get_server_port(current_server);

        if (tmp_conf->cert_path && tmp_conf->key_path) {
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                         "h3_child_init: found server %s:%d with H3 cert/key (defn:%s:%d)",
                         current_server->server_hostname ? current_server->server_hostname : "(default)",
                         port,
                         current_server->defn_name ? current_server->defn_name : "?",
                         current_server->defn_line_number);

            /* Only use the FIRST server we find with H3 config */
            if (!conf) {
                conf = tmp_conf;
                conf->host_port = port;
            }
        }
        current_server = current_server->next;
    }

    /* Go back and find which server we're actually using */
    if (conf) {
        current_server = s;
        while (current_server) {
            h3_server_conf* tmp_conf = ap_get_module_config(current_server->module_config, &http3_module);
            if (tmp_conf == conf) {
                break;
            }
            current_server = current_server->next;
        }
    }

    if (conf == NULL || !conf->cert_path || !conf->key_path) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
                     "h3_child_init: no cert/key configured, skipping QUIC listener");
        return;
    }

    /* Create this child's own UDP socket with SO_REUSEPORT */
    rv = apr_socket_create(&udp_sock, APR_INET6, SOCK_DGRAM, APR_PROTO_UDP, pchild);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "h3_child_init: Failed to create UDP socket");
        return;
    }

    apr_os_sock_get(&fd, udp_sock);

    /* Set SO_REUSEPORT - kernel routes packets deterministically per client */
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) < 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, errno, s,
                     "h3_child_init: Failed to set SO_REUSEPORT");
        apr_socket_close(udp_sock);
        return;
    }

    /* Bind to same address as parent's listener */
    rv = apr_sockaddr_info_get(&bind_addr, "::", APR_INET6, conf->host_port, 0, pchild);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "h3_child_init: Failed to get bind address");
        apr_socket_close(udp_sock);
        return;
    }

    rv = apr_socket_bind(udp_sock, bind_addr);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "h3_child_init: Failed to bind UDP socket");
        apr_socket_close(udp_sock);
        return;
    }

    ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                 "h3_child_init: PID %d created SO_REUSEPORT socket fd=%d on port %d",
                 getpid(), fd, conf->host_port);

    /* Update ALL h3 listeners to use this child's socket (includes duplicated ones) */
    int updated_count = 0;
    for (lr = ap_listeners; lr; lr = lr->next) {
        if (lr->protocol && strcmp(lr->protocol, "h3") == 0) {
            lr->sd = udp_sock;
            updated_count++;
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                         "h3_child_init: updated h3 listener %p to use UDP socket", (void*)lr);
        }
    }

    ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                 "h3_child_init: updated %d h3 listener(s) in ap_listeners", updated_count);

    /* Create QUIC listener data */
    ql = apr_pcalloc(pchild, sizeof(*ql));
    ql->pool = pchild;
    ql->server = current_server;  /* Use the server with H3 config, not the default server */
    ql->connections = apr_hash_make(pchild);

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                 "h3_child_init: using server=%s, defn_name=%s, defn_line_number=%d, port=%d for QUIC listener",
                 current_server->server_hostname ? current_server->server_hostname : "(null)",
                 current_server->defn_name ? current_server->defn_name : "(null)",
                 current_server->defn_line_number,
                 current_server->port);

    /* Create SSL context */
    ql->ssl_ctx = SSL_CTX_new(OSSL_QUIC_server_method());
    if (!ql->ssl_ctx) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "h3_child_init: Failed to create SSL_CTX");
        apr_socket_close(udp_sock);
        return;
    }

    /* Load certificate and key */
    if (SSL_CTX_use_certificate_chain_file(ql->ssl_ctx, conf->cert_path) <= 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "h3_child_init: Failed to load certificate: %s", conf->cert_path);
        SSL_CTX_free(ql->ssl_ctx);
        apr_socket_close(udp_sock);
        return;
    }

    if (SSL_CTX_use_PrivateKey_file(ql->ssl_ctx, conf->key_path, SSL_FILETYPE_PEM) <= 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "h3_child_init: Failed to load private key: %s", conf->key_path);
        SSL_CTX_free(ql->ssl_ctx);
        apr_socket_close(udp_sock);
        return;
    }

    /* Set ALPN callback to negotiate h3 */
    SSL_CTX_set_alpn_select_cb(ql->ssl_ctx, h3_alpn_select_cb, NULL);

    /* Create SSL listener */
    ql->ssl_listener = SSL_new_listener(ql->ssl_ctx, 0);
    if (!ql->ssl_listener) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "h3_child_init: Failed to create SSL listener");
        SSL_CTX_free(ql->ssl_ctx);
        apr_socket_close(udp_sock);
        return;
    }

    /* Create BIO for our UDP socket and attach to SSL listener */
    bio = BIO_new_dgram(fd, BIO_NOCLOSE);
    if (!bio) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "h3_child_init: Failed to create BIO for UDP socket");
        SSL_free(ql->ssl_listener);
        SSL_CTX_free(ql->ssl_ctx);
        apr_socket_close(udp_sock);
        return;
    }

    SSL_set_bio(ql->ssl_listener, bio, bio);

    /* Verify BIO fd matches our socket fd */
    int bio_fd = BIO_get_fd(bio, NULL);
    ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                 "h3_child_init: BIO fd=%d, socket fd=%d", bio_fd, fd);

    /* Start listening */
    if (!SSL_listen(ql->ssl_listener)) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "h3_child_init: SSL_listen failed");
        SSL_free(ql->ssl_listener);
        SSL_CTX_free(ql->ssl_ctx);
        apr_socket_close(udp_sock);
        return;
    }

    /* Set non-blocking mode */
    if (!SSL_set_blocking_mode(ql->ssl_listener, 0)) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "h3_child_init: Failed to set non-blocking mode");
        SSL_free(ql->ssl_listener);
        SSL_CTX_free(ql->ssl_ctx);
        apr_socket_close(udp_sock);
        return;
    }

    /* Store in child-specific global (can't use socket data - gets overwritten by other children!) */
    child_quic_listener = ql;

    /* Store UDP fd for thread */
    ql->udp_fd = fd;

    /* Create TCP dummy socket on localhost for wakeup
     * Thread will connect() to this to wake Apache MPM */
    apr_socket_t *dummy_sock;
    apr_sockaddr_t *dummy_addr;

    /* Create TCP socket on 127.0.0.1:0 (random port) */
    rv = apr_sockaddr_info_get(&dummy_addr, "127.0.0.1", APR_INET, 0, 0, pchild);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "h3_child_init: Failed to get dummy sockaddr");
        SSL_free(ql->ssl_listener);
        SSL_CTX_free(ql->ssl_ctx);
        apr_socket_close(udp_sock);
        return;
    }

    rv = apr_socket_create(&dummy_sock, APR_INET, SOCK_STREAM, APR_PROTO_TCP, pchild);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "h3_child_init: Failed to create dummy TCP socket");
        SSL_free(ql->ssl_listener);
        SSL_CTX_free(ql->ssl_ctx);
        apr_socket_close(udp_sock);
        return;
    }

    /* Make it reusable and non-blocking */
    apr_socket_opt_set(dummy_sock, APR_SO_REUSEADDR, 1);
    apr_socket_opt_set(dummy_sock, APR_SO_NONBLOCK, 1);

    /* Bind to random port */
    rv = apr_socket_bind(dummy_sock, dummy_addr);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "h3_child_init: Failed to bind dummy socket");
        apr_socket_close(dummy_sock);
        SSL_free(ql->ssl_listener);
        SSL_CTX_free(ql->ssl_ctx);
        apr_socket_close(udp_sock);
        return;
    }

    /* Put in LISTEN state - this is what MPM expects! */
    rv = apr_socket_listen(dummy_sock, 128);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "h3_child_init: Failed to listen on dummy socket");
        apr_socket_close(dummy_sock);
        SSL_free(ql->ssl_listener);
        SSL_CTX_free(ql->ssl_ctx);
        apr_socket_close(udp_sock);
        return;
    }

    /* Get the actual bound address (with assigned port) */
    apr_sockaddr_t *actual_addr;
    rv = apr_socket_addr_get(&actual_addr, APR_LOCAL, dummy_sock);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "h3_child_init: Failed to get dummy socket address");
        apr_socket_close(dummy_sock);
        SSL_free(ql->ssl_listener);
        SSL_CTX_free(ql->ssl_ctx);
        apr_socket_close(udp_sock);
        return;
    }

    /* Store for thread to connect to */
    ql->wakeup_listen_sock = dummy_sock;
    ql->wakeup_connect_addr = actual_addr;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                 "h3_child_init: created dummy TCP socket listening on 127.0.0.1:%d",
                 actual_addr->port);

    /* Update h3 listeners to use DUMMY SOCKET for wakeup
     * Apache MPM will monitor this LISTEN socket and call h3_quic_accept on connection */
    updated_count = 0;
    for (lr = ap_listeners; lr; lr = lr->next) {
        if (lr->protocol && strcmp(lr->protocol, "h3") == 0) {
            lr->sd = dummy_sock;  /* Apache monitors dummy TCP socket in LISTEN state! */
            updated_count++;
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                         "h3_child_init: updated h3 listener to use dummy TCP socket port %d",
                         actual_addr->port);
        }
    }

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                 "h3_child_init: configured %d h3 listeners with dummy socket", updated_count);

    /* Start background thread to process QUIC events */
    ql->thread_running = 1;
    apr_threadattr_t *thread_attr;
    apr_threadattr_create(&thread_attr, pchild);
    rv = apr_thread_create(&ql->event_thread, thread_attr, quic_event_thread, ql, pchild);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s,
                     "h3_child_init: Failed to create QUIC event thread");
        ql->thread_running = 0;
        return;
    }

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                 "h3_child_init: Started QUIC thread in PID %d, UDP socket in pollset",
                 getpid());
}
static void h3_c1_child_stopping(apr_pool_t* /*pool*/, int graceful)
{
    ap_log_error(APLOG_MARK, APLOG_TRACE8, 0, NULL, "h3_c1_child_stopping %d", graceful);
}
static int h3_hook_http_create_request(request_rec* r)
{
    const char* is_mod_http3 = apr_table_get(r->connection->notes, "IS_mod_http3");
    ap_log_rerror(APLOG_MARK, APLOG_TRACE8, 0, r, "h3_hook_http_create_request %s", is_mod_http3);
    if (is_mod_http3 == NULL)
        return DECLINED;

    ap_log_rerror(APLOG_MARK, APLOG_TRACE8, 0, r, "h3_hook_http_create_request status %d", r->status);
    if (r->main != NULL)
    {
        return DECLINED;
    }

    /* Add the filter for the response here */
    ap_add_input_filter_handle(h3_proto_in_filter_handle, NULL, r, r->connection);
    ap_add_input_filter_handle(h3_net_in_filter_handle, NULL, NULL, r->connection);
    ap_add_output_filter_handle(h3_net_out_filter_handle, NULL, NULL, r->connection);

    return OK;
}
static void h3_filter_last(request_rec* r)
{
    const char* is_mod_http3 = apr_table_get(r->connection->notes, "IS_mod_http3");
    ap_log_rerror(APLOG_MARK, APLOG_TRACE8, 0, r, "h3_filter_last %s", is_mod_http3);
    if (is_mod_http3 == NULL)
        return;
    ap_add_output_filter_handle(h3_proto_out_filter_handle, NULL, r, r->connection); /* HACKING */
}

/* Configuration directives */
static const command_rec h3_cmds[] = {AP_INIT_TAKE1("H3CertificatePath", set_h3_cert_path, NULL, RSRC_CONF, "Path to the SSL certificate file for HTTP/3"), AP_INIT_TAKE1("H3CertificateKeyPath", set_h3_key_path, NULL, RSRC_CONF, "Path to the SSL certificate key file for HTTP/3"), {NULL}};

/* Hook into connection creation to handle QUIC streams from MPM */
static conn_rec *h3_create_connection(apr_pool_t *ptrans, server_rec *server,
                                       apr_socket_t *csd, long conn_id,
                                       void *sbh, apr_bucket_alloc_t *alloc)
{
    h3_quic_conn_state_t *qcs = NULL;
    apr_status_t rv;

    /* Try to get our h3 state from the socket's user data */
    rv = apr_socket_data_get((void **)&qcs, "h3_state", csd);
    if (rv != APR_SUCCESS || !qcs || qcs->magic != H3_QUIC_MAGIC) {
        /* Not a QUIC connection, let core handler create it */
        ap_log_error(APLOG_MARK, APLOG_TRACE8, 0, server,
                     "h3_create_connection: not QUIC (rv=%d, qcs=%p, magic=%08x), passing to core",
                     rv, (void*)qcs, qcs ? qcs->magic : 0);
        return NULL;
    }

    /* Try to get the h3_stream from the socket's user data */
    h3_stream *stream = NULL;
    apr_socket_data_get((void **)&stream, "h3_stream", csd);

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, server,
                 "h3_create_connection: QUIC stream detected (stream_id=%lu), creating conn_rec",
                 stream ? (unsigned long)stream->stream_id : 0UL);

    /* Create the connection record for this HTTP/3 stream/request */
    conn_rec *c = apr_pcalloc(ptrans, sizeof(conn_rec));
    apr_sockaddr_t *fake_local, *fake_client;

    c->pool = ptrans;
    /* Use the server from the h3_session, not the one MPM passes (which is wrong port) */
    c->base_server = stream && stream->session ? stream->session->s : server;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, server,
                 "h3_create_connection: using base_server=%s:%d (from %s), MPM passed %s:%d, stream=%p session=%p",
                 c->base_server->server_hostname ? c->base_server->server_hostname : "(default)",
                 c->base_server->port,
                 (stream && stream->session) ? "stream->session->s" : "MPM server parameter",
                 server->server_hostname ? server->server_hostname : "(default)",
                 server->port,
                 (void*)stream,
                 stream ? (void*)stream->session : NULL);

    /* Create dummy addresses to prevent crashes - TODO: extract from QUIC SSL */
    apr_sockaddr_info_get(&fake_local, "127.0.0.1", APR_INET, 4433, 0, ptrans);
    /* Use a non-zero client port - some authz modules reject port 0 */
    apr_sockaddr_info_get(&fake_client, "127.0.0.1", APR_INET, 12345, 0, ptrans);

    c->local_addr = fake_local;
    c->client_addr = fake_client;
    c->local_ip = apr_pstrdup(ptrans, "127.0.0.1");
    c->client_ip = apr_pstrdup(ptrans, "127.0.0.1");
    c->conn_config = ap_create_conn_config(ptrans);
    c->notes = apr_table_make(ptrans, 5);
    c->id = conn_id;
    c->bucket_alloc = alloc;
    c->sbh = sbh;
    c->keepalives = 0;
    c->log = NULL;
    c->aborted = 0;  /* Don't set aborted yet - let connection process first */
    c->input_filters = NULL;
    c->output_filters = NULL;
    c->keepalive = AP_CONN_CLOSE;  /* Don't try to keep alive QUIC streams */
    c->clogging_input_filters = 1;  /* Prevent socket operations */

    /* Store QUIC state in connection config */
    ap_set_module_config(c->conn_config, &http3_module, qcs);

    /* Store the h3_stream pointer if we have one */
    if (stream) {
        apr_table_setn(c->notes, "h3_stream", (const char *)stream);
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server,
                     "h3_create_connection: attached h3_stream %lu to conn_rec",
                     (unsigned long)stream->stream_id);
    }

    /* Mark this as a QUIC/HTTP3 connection */
    apr_table_set(c->notes, "IS_mod_http3", "1");

    /* Tell mod_ssl to skip this connection - QUIC has TLS built-in */
    apr_table_setn(c->notes, "ssl-bypass", "1");

    /* CRITICAL: Set master to non-NULL so mod_ssl skips SSL setup */
    c->master = c;  /* Point to itself - simple way to be non-NULL */
    c->cs = NULL;
    c->remote_host = apr_pstrdup(ptrans, "localhost");

    /* CRITICAL FIX: Remove any SSL filters that might have been inherited or added.
     * HTTP/3 uses QUIC which handles TLS, so we don't need mod_ssl's filters.
     * We must do this BEFORE ap_update_vhost_given_ip and other operations. */
    ap_remove_input_filter_byhandle(c->input_filters, "ssl_io_filter");
    ap_remove_output_filter_byhandle(c->output_filters, "ssl_io_filter");
    ap_remove_output_filter_byhandle(c->output_filters, "ssl_io_coalesce");

    /* Use the dummy socket for core module config so Apache can do operations
     * The dummy socket is a real socketpair that Apache can set options on,
     * but we'll do actual I/O through our QUIC SSL streams in the filters */
    ap_set_core_module_config(c->conn_config, qcs->dummy_sock);

    /* Update virtual host based on IP:port in c->local_addr before request processing */
    ap_update_vhost_given_ip(c);

    /* Install I/O filters for QUIC stream */
    ap_add_input_filter_handle(h3_net_in_filter_handle, NULL, NULL, c);
    ap_add_output_filter_handle(h3_net_out_filter_handle, NULL, NULL, c);

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, server,
                 "h3_create_connection: after ap_update_vhost_given_ip, base_server=%s:%d (local_addr port=%d)",
                 c->base_server->server_hostname ? c->base_server->server_hostname : "(default)",
                 c->base_server->port,
                 c->local_addr ? c->local_addr->port : -1);

    return c;
}

static int h3_hook_pre_close_connection(conn_rec* c)
{
    const char* is_mod_http3 = apr_table_get(c->notes, "IS_mod_http3");
    if (is_mod_http3) {
        ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, c,
                      "h3_hook_pre_close_connection: QUIC connection closing");

        /* Apache can close the dummy socket normally - it's a real socketpair */
        /* Clean up QUIC connection - check if client sent CONNECTION_CLOSE */
        h3_quic_conn_state_t *qcs = ap_get_module_config(c->conn_config, &http3_module);
        if (qcs && qcs->ssl_conn) {
            /* Get connection close info from OpenSSL */
            SSL_CONN_CLOSE_INFO close_info = {0};
            if (SSL_get_conn_close_info(qcs->ssl_conn, &close_info, sizeof(close_info)) == 1) {
                ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, c,
                              "h3_hook_pre_close_connection: CONNECTION_CLOSE - "
                              "error_code=%lu, flags=0x%x (LOCAL=%d TRANSPORT=%d), reason=%.*s",
                              (unsigned long)close_info.error_code,
                              close_info.flags,
                              !!(close_info.flags & SSL_CONN_CLOSE_FLAG_LOCAL),
                              !!(close_info.flags & SSL_CONN_CLOSE_FLAG_TRANSPORT),
                              (int)close_info.reason_len,
                              close_info.reason ? close_info.reason : "");
            } else {
                ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, c,
                              "h3_hook_pre_close_connection: no CONNECTION_CLOSE info available");
            }

            /* Free the SSL connection - this will send CONNECTION_CLOSE if not already sent */
            ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, c,
                          "h3_hook_pre_close_connection: freeing SSL connection (will send CONNECTION_CLOSE)");
            SSL_free(qcs->ssl_conn);
            qcs->ssl_conn = NULL;
        }

        return OK;
    }
    return DECLINED;
}

/* Per-directory config - required for access_checker hook to work */
static void *h3_create_dir_config(apr_pool_t *p, char *dir)
{
    /* Return a non-NULL pointer so Apache knows we have config for this directory.
     * We don't actually need any config data, just need to exist in the per_dir_config. */
    return apr_pcalloc(p, 1);
}

static void *h3_merge_dir_config(apr_pool_t *p, void *base, void *add)
{
    /* No actual config to merge, just return something non-NULL */
    return apr_pcalloc(p, 1);
}

static void register_hooks(apr_pool_t* p)
{
    ap_log_perror(APLOG_MARK, APLOG_ERR, 0, p,
                  "register_hooks: REGISTERING h3_hook_access_checker at %p", (void*)h3_hook_access_checker);

    ap_hook_post_config(h3_post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_create_connection(h3_create_connection, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_pre_connection(h3_hook_pre_connection, NULL, NULL, APR_HOOK_REALLY_FIRST);
    ap_hook_pre_close_connection(h3_hook_pre_close_connection, NULL, NULL, APR_HOOK_REALLY_FIRST);
    ap_hook_process_connection(h3_hook_process_connection, NULL, NULL, APR_HOOK_REALLY_FIRST);
    ap_hook_create_request(h3_hook_http_create_request, NULL, NULL, APR_HOOK_REALLY_FIRST);
    ap_hook_pre_read_request(h3_hook_pre_read_request, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_read_request(h3_hook_post_read_request, NULL, NULL, APR_HOOK_REALLY_FIRST);
    /* Register access_checker hook - REQUIRED to return OK */
    ap_hook_access_checker(h3_hook_access_checker, NULL, NULL, APR_HOOK_REALLY_FIRST);

    /* Register a SECOND hook to test if hooks are being called */
    ap_hook_access_checker(h3_hook_access_checker2, NULL, NULL, APR_HOOK_MIDDLE);

    /* ALSO register check_access_ex hook to bypass authz_core's "Require" directives */
    ap_hook_access_checker_ex(h3_hook_access_checker, NULL, NULL, APR_HOOK_REALLY_FIRST);
    h3_net_out_filter_handle = ap_register_output_filter("H3_NET_OUT", h3_filter_out, NULL, AP_FTYPE_NETWORK);
    h3_net_in_filter_handle = ap_register_input_filter("H3_NET_IN", h3_filter_in, NULL, AP_FTYPE_NETWORK);

    h3_proto_out_filter_handle = ap_register_output_filter("H3_NET_OUT_PROTO", h3_filter_out_proto, NULL, AP_FTYPE_PROTOCOL);

    h3_proto_in_filter_handle = ap_register_input_filter("H3_NET_IN_PROTO", h3_filter_in_proto, NULL, AP_FTYPE_PROTOCOL);
    ap_hook_insert_filter(h3_filter_last, NULL, NULL, APR_HOOK_LAST);

    ap_hook_child_init(h3_child_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_stopping(h3_c1_child_stopping, NULL, NULL, APR_HOOK_MIDDLE);
#ifdef AP_HAS_RESPONSE_BUCKETS
    #error Not supported for the moment.
#endif
}

AP_DECLARE_MODULE(http3) = {
    STANDARD20_MODULE_STUFF,
    h3_create_dir_config,    /* create per-directory config structure */
    h3_merge_dir_config,     /* merge per-directory config structures */
    h3_create_server_config, /* create per-server config structure */
    h3_merge_server_config,  /* merge per-server config structures */
    h3_cmds,                 /* command apr_table_t */
    register_hooks,          /* register hooks */
    AP_MODULE_FLAG_NONE      /* flags */
};
