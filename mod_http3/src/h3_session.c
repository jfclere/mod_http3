/*
 * h3_session.c - HTTP/3 session management
 */

#include "h3_session.h"
#include <http_log.h>
#include <http_protocol.h>
#include <apr_strings.h>
#include <apr_uri.h>
#include <apr_thread_proc.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <fcntl.h>

/* Forward declaration - need to access http3_module and h3_quic_conn_state_t */
extern module AP_MODULE_DECLARE_DATA http3_module;

typedef struct {
    apr_uint32_t magic;
    SSL *ssl_conn;
    SSL *ssl_stream;
    apr_uint64_t stream_id;
    apr_pool_t *pool;
    void *listener;
} h3_quic_conn_state_t;

/* UDP socket monitor thread - bridges QUIC socket events to Apache's event loop */
static void *APR_THREAD_FUNC udp_monitor_thread(apr_thread_t *thd, void *arg)
{
    h3_session *session = (h3_session *)arg;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: UDP monitor thread started, monitoring fd=%d", session->udp_fd);

    while (1) {
        /* Check if we should exit */
        apr_thread_mutex_lock(session->mutex);
        int should_run = session->monitor_running;
        apr_thread_mutex_unlock(session->mutex);

        if (!should_run) {
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                          "h3_session: UDP monitor thread exiting");
            break;
        }

        /* Wait for UDP socket activity with timeout */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(session->udp_fd, &rfds);

        struct timeval tv = {0, 500000};  /* 500ms timeout for responsive shutdown */
        int select_ret = select(session->udp_fd + 1, &rfds, NULL, NULL, &tv);

        if (select_ret > 0 && FD_ISSET(session->udp_fd, &rfds)) {
            /* UDP data arrived! Wake up Apache by writing to the pipe */
            char wake_byte = 'U';  /* 'U' for UDP data */
            ssize_t written = write(session->wakeup_pipe[1], &wake_byte, 1);

            if (written != 1) {
                ap_log_error(APLOG_MARK, APLOG_ERR, errno, session->s,
                              "h3_session: failed to write wake-up byte");
            } else {
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, session->s,
                              "h3_session: UDP activity detected, woke up Apache");
            }
        } else if (select_ret < 0 && errno != EINTR) {
            ap_log_error(APLOG_MARK, APLOG_ERR, errno, session->s,
                          "h3_session: select() failed on UDP socket");
            break;
        }
        /* select_ret == 0 means timeout, loop again */
    }

    apr_thread_exit(thd, APR_SUCCESS);
    return NULL;
}

/* nghttp3 callbacks */
static int on_begin_headers(nghttp3_conn *conn, int64_t stream_id, void *user_data,
                           void *stream_user_data)
{
    (void)stream_user_data;
    h3_session *session = user_data;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: begin headers on stream %ld", (long)stream_id);

    /* Check if stream already exists (created by SSL_accept_stream) */
    apr_uint64_t lookup_id = (apr_uint64_t)stream_id;
    h3_stream *stream = apr_hash_get(session->streams, &lookup_id, sizeof(lookup_id));

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: on_begin_headers - stream %ld %s in hash, ssl_stream=%s",
                  (long)stream_id, stream ? "FOUND" : "NOT FOUND",
                  stream ? (stream->ssl_stream ? "SET" : "NULL") : "N/A");

    if (!stream) {
        /* Create a new stream if it doesn't exist yet */
        stream = apr_pcalloc(session->pool, sizeof(h3_stream));
        stream->session = session;
        stream->stream_id = (apr_uint64_t)stream_id;
        stream->is_bidi = ((stream_id & 0x2) == 0);
        stream->headers_complete = 0;
        stream->request_complete = 0;

        /* Store in session */
        apr_uint64_t *key = apr_pmemdup(session->pool, &stream->stream_id, sizeof(stream->stream_id));
        apr_hash_set(session->streams, key, sizeof(*key), stream);
    }

    /* For bidirectional streams, create table to store headers temporarily */
    if (stream->is_bidi && !stream->headers) {
        stream->r = NULL;
        stream->method = NULL;
        stream->scheme = NULL;
        stream->authority = NULL;
        stream->path = NULL;
        stream->headers = apr_table_make(session->pool, 10);
    }

    /* Tell nghttp3 about this stream */
    nghttp3_conn_set_stream_user_data(conn, stream_id, stream);

    return 0;
}

static int on_recv_header(nghttp3_conn *conn, int64_t stream_id,
                         int32_t token, nghttp3_rcbuf *name, nghttp3_rcbuf *value,
                         uint8_t flags, void *user_data, void *stream_user_data)
{
    (void)conn;
    (void)flags;
    h3_stream *stream = stream_user_data;
    h3_session *session = user_data;

    if (!stream) {
        return 0;
    }

    nghttp3_vec namevec = nghttp3_rcbuf_get_buf(name);
    nghttp3_vec valuevec = nghttp3_rcbuf_get_buf(value);

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: header on stream %lu: %.*s = %.*s",
                  (unsigned long)stream_id,
                  (int)namevec.len, namevec.base,
                  (int)valuevec.len, valuevec.base);

    /* Store HTTP/3 pseudo-headers in h3_stream (will populate request_rec later) */

    /* :path */
    if (token == NGHTTP3_QPACK_TOKEN__PATH) {
        if (valuevec.len == 0 || valuevec.len >= 8192) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, session->s,
                        "Path too long or empty: %zu bytes", valuevec.len);
            return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
        }
        stream->path = apr_pstrndup(session->pool, (const char*)valuevec.base, valuevec.len);
        return 0;
    }

    /* :scheme */
    if (token == NGHTTP3_QPACK_TOKEN__SCHEME) {
        if (valuevec.len == 0 || valuevec.len >= 8192) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, session->s,
                        "Scheme too long or empty: %zu bytes", valuevec.len);
            return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
        }
        stream->scheme = apr_pstrndup(session->pool, (const char*)valuevec.base, valuevec.len);
        return 0;
    }

    /* :method */
    if (token == NGHTTP3_QPACK_TOKEN__METHOD) {
        if (valuevec.len == 0 || valuevec.len >= 8192) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, session->s,
                        "Invalid method length: %zu", valuevec.len);
            return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
        }
        stream->method = apr_pstrndup(session->pool, (const char*)valuevec.base, valuevec.len);
        return 0;
    }

    /* :authority = Host */
    if (token == NGHTTP3_QPACK_TOKEN__AUTHORITY) {
        if (valuevec.len == 0 || valuevec.len >= 8192) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, session->s,
                        "Authority too long or empty: %zu bytes", valuevec.len);
            return NGHTTP3_ERR_MALFORMED_HTTP_HEADER;
        }
        stream->authority = apr_pstrndup(session->pool, (const char*)valuevec.base, valuevec.len);

        /* Check for non-printable characters */
        char hex_dump[128] = {0};
        for (size_t i = 0; i < valuevec.len && i < 30; i++) {
            apr_snprintf(hex_dump + (i * 3), sizeof(hex_dump) - (i * 3), "%02x ", (unsigned char)valuevec.base[i]);
        }

        ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                     "h3_session: stored :authority = '%s' (len=%" APR_SIZE_T_FMT ", raw bytes: %s)",
                     stream->authority, strlen(stream->authority), hex_dump);
        return 0;
    }

    /* Regular headers */
    char *hname = apr_pstrndup(session->pool, (const char*)namevec.base, namevec.len);
    char *hvalue = apr_pstrndup(session->pool, (const char*)valuevec.base, valuevec.len);
    apr_table_addn(stream->headers, hname, hvalue);

    return 0;
}

static int on_end_headers(nghttp3_conn *conn, int64_t stream_id,
                         int fin, void *user_data, void *stream_user_data)
{
    h3_stream *stream = stream_user_data;
    h3_session *session = user_data;

    if (!stream) {
        return 0;
    }

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: end headers on stream %lu, fin=%d, ssl_stream=%s",
                  (unsigned long)stream_id, fin, stream->ssl_stream ? "SET" : "NULL");

    stream->headers_complete = 1;

    /* Tell nghttp3 we're done reading from this stream (whether or not client sent FIN) */
    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: shutting down read on stream %lu (client fin=%d)",
                  (unsigned long)stream_id, fin);

    int rv = nghttp3_conn_shutdown_stream_read(conn, stream_id);
    if (rv != 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, session->s,
                      "h3_session: nghttp3_conn_shutdown_stream_read failed: %d", rv);
    }

    stream->request_complete = 1;

    /* Add to ready queue */
    APR_ARRAY_PUSH(session->ready_streams, h3_stream *) = stream;
    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: stream %lu request complete (headers done), added to ready queue",
                  (unsigned long)stream_id);

    return 0;
}

static int on_recv_data(nghttp3_conn *conn, int64_t stream_id,
                       const uint8_t *data, size_t datalen,
                       void *user_data, void *stream_user_data)
{
    (void)conn;
    (void)data;
    h3_stream *stream = stream_user_data;
    h3_session *session = user_data;

    if (!stream) {
        return 0;
    }

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: received %zu bytes of data on stream %lu",
                  datalen, (unsigned long)stream_id);

    /* TODO: Buffer request body data */

    return 0;
}

static int on_acked_stream_data(nghttp3_conn *conn, int64_t stream_id,
                                uint64_t datalen, void *user_data,
                                void *stream_user_data)
{
    h3_session *session = user_data;
    (void)conn;
    (void)stream_user_data;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: %lu bytes ACKed on stream %ld",
                  (unsigned long)datalen, (long)stream_id);

    /* Tell nghttp3 the data was consumed */
    int rv = nghttp3_conn_add_ack_offset(conn, stream_id, datalen);
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, session->s,
                      "h3_session: nghttp3_conn_add_ack_offset failed: %d", rv);
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    }

    return 0;
}

static int on_stop_sending(nghttp3_conn *conn, int64_t stream_id,
                           uint64_t app_error_code,
                           void *user_data, void *stream_user_data)
{
    h3_session *session = user_data;
    (void)conn;
    (void)stream_user_data;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: STOP_SENDING received on stream %ld, error_code=%lu",
                  (long)stream_id, (unsigned long)app_error_code);

    /* Client wants us to stop sending on this stream */
    return 0;
}

static int on_reset_stream(nghttp3_conn *conn, int64_t stream_id,
                           uint64_t app_error_code,
                           void *user_data, void *stream_user_data)
{
    h3_session *session = user_data;
    (void)conn;
    (void)stream_user_data;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: RESET_STREAM received on stream %ld, error_code=%lu",
                  (long)stream_id, (unsigned long)app_error_code);

    /* Client is resetting this stream */
    return 0;
}

static int on_stream_close(nghttp3_conn *conn, int64_t stream_id,
                          uint64_t app_error_code, void *user_data,
                          void *stream_user_data)
{
    (void)conn;
    h3_stream *stream = stream_user_data;
    h3_session *session = user_data;

    /* This callback is fired by nghttp3 when a stream is closed */
    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: *** CALLBACK FIRED *** on_stream_close stream=%ld",
                  (long)stream_id);

    if (!stream) {
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                      "h3_session: on_stream_close - stream pointer is NULL!");
        return 0;
    }

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: CALLBACK on_stream_close - stream %lu closed, error_code=%lu",
                  (unsigned long)stream_id, (unsigned long)app_error_code);

    /* If headers were complete but request wasn't marked complete yet, do it now */
    if (stream->headers_complete && !stream->request_complete) {
        stream->request_complete = 1;
        APR_ARRAY_PUSH(session->ready_streams, h3_stream *) = stream;
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                      "h3_session: stream %lu closed with complete headers, added to ready queue",
                      (unsigned long)stream_id);
    }

    /* Check if this is a bidirectional stream (request stream) that just closed.
     * The stream close event confirms both sides are done - we sent FIN, client sent FIN. */
    if ((stream_id & 0x2) == 0) {  /* Bidirectional stream */
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                      "h3_session: Request stream %lu FULLY closed (both sides done)",
                      (unsigned long)stream_id);

        /* Initiate graceful shutdown immediately */
        if (!session->goaway_queued) {
            int rv = nghttp3_conn_submit_shutdown_notice(session->ngh3);
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                          "h3_session: stream fully closed, submitted shutdown notice rv=%d",
                          rv);

            rv = nghttp3_conn_shutdown(session->ngh3);
            session->goaway_queued = 1;
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                          "h3_session: called nghttp3_conn_shutdown rv=%d - GOAWAY queued",
                          rv);
        }
    }

    return 0;
}

apr_status_t h3_session_create(h3_session **psession,
                                server_rec *s,
                                SSL *ssl_listener,
                                SSL *ssl_conn,
                                apr_pool_t *pool)
{
    h3_session *session;
    nghttp3_callbacks callbacks = {0};
    nghttp3_settings settings;
    int rv;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                  "h3_session_create: START");

    session = apr_pcalloc(pool, sizeof(*session));
    session->c = NULL;  /* No conn_rec yet - created when request ready */
    session->s = s;
    session->pool = pool;
    session->ssl_listener = ssl_listener;  /* Store listener for accepting streams */
    session->ssl_conn = ssl_conn;
    session->streams = apr_hash_make(pool);
    session->ready_streams = apr_array_make(pool, 4, sizeof(h3_stream *));
    session->aborted = 0;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                  "h3_session_create: allocated session struct");

    /* Set up nghttp3 callbacks */
    callbacks.acked_stream_data = on_acked_stream_data;  /* Track when data is ACKed */
    callbacks.recv_header = on_recv_header;
    callbacks.end_headers = on_end_headers;
    callbacks.recv_data = on_recv_data;
    callbacks.stream_close = on_stream_close;
    callbacks.begin_headers = on_begin_headers;
    callbacks.stop_sending = on_stop_sending;
    callbacks.reset_stream = on_reset_stream;

    /* Initialize nghttp3 settings with defaults */
    nghttp3_settings_default(&settings);

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                  "h3_session_create: calling nghttp3_conn_server_new");

    /* Create nghttp3 connection (server side) */
    const nghttp3_mem *mem = nghttp3_mem_default();
    rv = nghttp3_conn_server_new(&session->ngh3, &callbacks, &settings, mem, session);
    if (rv != 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                      "h3_session: nghttp3_conn_server_new failed: %d", rv);
        return APR_EGENERAL;
    }

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                  "h3_session_create: nghttp3 connection created, now creating SSL streams");

    /* Initialize control streams flag - will be created when SSL_POLL_EVENT_OSU fires */
    session->control_streams_created = 0;

    *psession = session;
    return APR_SUCCESS;
}

/* Create control and QPACK streams - called when SSL_POLL_EVENT_OSU fires */
apr_status_t h3_session_create_control_streams(h3_session *session)
{
    server_rec *s = session->s;
    SSL *ssl_conn = session->ssl_conn;

    if (session->control_streams_created) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
                      "h3_session_create_control_streams: already created, skipping");
        return APR_SUCCESS;
    }

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                  "h3_session_create_control_streams: creating control/QPACK streams");

    /* Create the 3 required unidirectional streams for HTTP/3:
     * - Control stream
     * - QPACK encoder stream
     * - QPACK decoder stream */
    SSL *control_stream = SSL_new_stream(ssl_conn, SSL_STREAM_FLAG_UNI);
    if (!control_stream) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                      "h3_session_create: SSL_new_stream(control) failed: %s", err_buf);
    } else {
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                      "h3_session_create: created control_stream=%p", (void*)control_stream);
    }

    SSL *qpack_enc_stream = SSL_new_stream(ssl_conn, SSL_STREAM_FLAG_UNI);
    if (!qpack_enc_stream) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                      "h3_session_create: SSL_new_stream(qpack_enc) failed: %s", err_buf);
    } else {
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                      "h3_session_create: created qpack_enc_stream=%p", (void*)qpack_enc_stream);
    }

    SSL *qpack_dec_stream = SSL_new_stream(ssl_conn, SSL_STREAM_FLAG_UNI);
    if (!qpack_dec_stream) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                      "h3_session_create: SSL_new_stream(qpack_dec) failed: %s", err_buf);
    } else {
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                      "h3_session_create: created qpack_dec_stream=%p", (void*)qpack_dec_stream);
    }

    if (!control_stream || !qpack_enc_stream || !qpack_dec_stream) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                      "h3_session: failed to create one or more unidirectional streams (control=%p, qpack_enc=%p, qpack_dec=%p)",
                      (void*)control_stream, (void*)qpack_enc_stream, (void*)qpack_dec_stream);
        if (control_stream) SSL_free(control_stream);
        if (qpack_enc_stream) SSL_free(qpack_enc_stream);
        if (qpack_dec_stream) SSL_free(qpack_dec_stream);
        nghttp3_conn_del(session->ngh3);
        return APR_EGENERAL;
    }

    /* Set streams to non-blocking mode */
    SSL_set_blocking_mode(control_stream, 0);
    SSL_set_blocking_mode(qpack_enc_stream, 0);
    SSL_set_blocking_mode(qpack_dec_stream, 0);

    int64_t control_id = (int64_t)SSL_get_stream_id(control_stream);
    int64_t qpack_enc_id = (int64_t)SSL_get_stream_id(qpack_enc_stream);
    int64_t qpack_dec_id = (int64_t)SSL_get_stream_id(qpack_dec_stream);

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                  "h3_session: created streams - control=%ld, qpack_enc=%ld, qpack_dec=%ld",
                  (long)control_id, (long)qpack_enc_id, (long)qpack_dec_id);

    /* Bind the streams to nghttp3 */
    int ngh3_rv = nghttp3_conn_bind_control_stream(session->ngh3, control_id);
    if (ngh3_rv != 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                      "h3_session: nghttp3_conn_bind_control_stream failed: %d", ngh3_rv);
        return APR_EGENERAL;
    }

    ngh3_rv = nghttp3_conn_bind_qpack_streams(session->ngh3, qpack_enc_id, qpack_dec_id);
    if (ngh3_rv != 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                      "h3_session: nghttp3_conn_bind_qpack_streams failed: %d", ngh3_rv);
        return APR_EGENERAL;
    }

    /* Store the streams in the session */
    h3_stream *control_h3s = apr_pcalloc(session->pool, sizeof(h3_stream));
    control_h3s->session = session;
    control_h3s->stream_id = (apr_uint64_t)control_id;
    control_h3s->ssl_stream = control_stream;
    control_h3s->is_bidi = 0;
    apr_hash_set(session->streams, &control_h3s->stream_id, sizeof(control_h3s->stream_id), control_h3s);

    h3_stream *enc_h3s = apr_pcalloc(session->pool, sizeof(h3_stream));
    enc_h3s->session = session;
    enc_h3s->stream_id = (apr_uint64_t)qpack_enc_id;
    enc_h3s->ssl_stream = qpack_enc_stream;
    enc_h3s->is_bidi = 0;
    apr_hash_set(session->streams, &enc_h3s->stream_id, sizeof(enc_h3s->stream_id), enc_h3s);

    h3_stream *dec_h3s = apr_pcalloc(session->pool, sizeof(h3_stream));
    dec_h3s->session = session;
    dec_h3s->stream_id = (apr_uint64_t)qpack_dec_id;
    dec_h3s->ssl_stream = qpack_dec_stream;
    dec_h3s->is_bidi = 0;
    apr_hash_set(session->streams, &dec_h3s->stream_id, sizeof(dec_h3s->stream_id), dec_h3s);

    /* Mark control streams as created */
    session->control_streams_created = 1;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                  "h3_session_create_control_streams: successfully created all control streams");

    return APR_SUCCESS;
}

apr_status_t h3_session_process(h3_session *session, h3_stream *specific_stream)
{
    SSL *stream;
    int processed = 0;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: processing session - START (called by thread after SSL_poll), specific_stream=%p",
                  (void*)specific_stream);

    /* Thread already called SSL_handle_events() - don't call it again!
     * Just accept and process streams that SSL_poll indicated are ready */

    /* If specific_stream is given, read from THAT stream. Otherwise accept NEW streams */
    if (specific_stream) {
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                      "h3_session: reading from specific stream %lu",
                      (unsigned long)specific_stream->stream_id);

        /* Read data from this specific stream and feed to nghttp3 */
        unsigned char buffer[8192];
        size_t bytes_read = 0;
        int read_ret = SSL_read_ex(specific_stream->ssl_stream, buffer, sizeof(buffer), &bytes_read);

        if (read_ret > 0 && bytes_read > 0) {
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                          "h3_session: read %zu bytes from stream %lu, feeding to nghttp3",
                          bytes_read, (unsigned long)specific_stream->stream_id);

            nghttp3_ssize nconsumed = nghttp3_conn_read_stream(
                session->ngh3, specific_stream->stream_id,
                buffer, bytes_read, 0);

            if (nconsumed < 0) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, session->s,
                              "h3_session: nghttp3_conn_read_stream failed: %ld",
                              (long)nconsumed);
                return APR_EGENERAL;
            }

            ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                          "h3_session: nghttp3 consumed %ld bytes from stream %lu",
                          (long)nconsumed, (unsigned long)specific_stream->stream_id);
        } else {
            int ssl_error = SSL_get_error(specific_stream->ssl_stream, read_ret);
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                          "h3_session: SSL_read_ex returned %d, ssl_error=%d, bytes_read=%zu",
                          read_ret, ssl_error, bytes_read);
        }

        return APR_SUCCESS;
    }

    /* No specific stream - try to accept NEW streams */
    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: attempting to accept more streams");

    while ((stream = SSL_accept_stream(session->ssl_conn, SSL_ACCEPT_STREAM_NO_BLOCK)) != NULL) {
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                      "h3_session: SSL_accept_stream returned stream %p", (void*)stream);
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                      "h3_session: SSL_accept_stream returned a stream");

        /* Set stream to non-blocking mode */
        SSL_set_blocking_mode(stream, 0);

        apr_uint64_t stream_id = SSL_get_stream_id(stream);
        int is_bidi = ((stream_id & 0x2) == 0);

        if (!stream) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, session->s,
                          "h3_session: BUG - SSL_accept_stream returned NULL stream but we got here!");
            continue;
        }

        ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                      "h3_session: accepted stream %lu, is_bidi=%d, stream is NOT NULL",
                      (unsigned long)stream_id, is_bidi);

        /* Create or update h3_stream BEFORE feeding data to nghttp3
         * (nghttp3 callbacks need to find it in the hash) */
        h3_stream *h3s = apr_hash_get(session->streams, &stream_id, sizeof(stream_id));
        if (!h3s) {
            /* New stream - create h3_stream and add to hash */
            h3s = apr_pcalloc(session->pool, sizeof(h3_stream));
            h3s->session = session;
            h3s->stream_id = stream_id;
            h3s->ssl_stream = stream;
            h3s->is_bidi = is_bidi;
            h3s->headers_complete = 0;
            h3s->request_complete = 0;

            apr_uint64_t *stream_key = apr_pmemdup(session->pool, &stream_id, sizeof(stream_id));
            apr_hash_set(session->streams, stream_key, sizeof(*stream_key), h3s);

            ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                          "h3_session: created new h3_stream for stream %lu (bidi=%d), ssl_stream=%s",
                          (unsigned long)stream_id, is_bidi, h3s->ssl_stream ? "SET" : "NULL");
        } else {
            /* Existing stream - just update SSL stream pointer */
            h3s->ssl_stream = stream;
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                          "h3_session: updated existing h3_stream for stream %lu, ssl_stream=%p",
                          (unsigned long)stream_id, (void*)h3s->ssl_stream);
        }

        /* Read data from stream and feed to nghttp3 */
        unsigned char buffer[8192];
        size_t bytes_read = 0;
        int read_ret = SSL_read_ex(stream, buffer, sizeof(buffer), &bytes_read);

        if (read_ret > 0 && bytes_read > 0) {
            /* Check if stream received FIN (stream read side is finished) */
            int read_state = SSL_get_stream_read_state(stream);
            int fin = (read_state == SSL_STREAM_STATE_FINISHED) ? 1 : 0;

            ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                          "h3_session: read %zu bytes from stream %lu, read_state=%d, fin=%d",
                          bytes_read, (unsigned long)stream_id, read_state, fin);

            /* Feed data to nghttp3 with FIN flag if stream is finished */
            nghttp3_ssize nconsumed = nghttp3_conn_read_stream(
                session->ngh3,
                (int64_t)stream_id,
                buffer,
                bytes_read,
                fin
            );

            if (nconsumed < 0) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, session->s,
                              "h3_session: nghttp3_conn_read_stream failed: %ld on stream %lu",
                              (long)nconsumed, (unsigned long)stream_id);
            } else {
                ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                              "h3_session: nghttp3 consumed %ld bytes from stream %lu",
                              (long)nconsumed, (unsigned long)stream_id);
            }
        } else if (read_ret == 0) {
            int ssl_err = SSL_get_error(stream, read_ret);
            if (ssl_err != SSL_ERROR_WANT_READ) {
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, session->s,
                              "h3_session: SSL_read_ex on stream %lu: ssl_err=%d",
                              (unsigned long)stream_id, ssl_err);
            }
        }

        processed++;
    }

    /* Log if SSL_accept_stream returned NULL */
    if (processed == 0) {
        int err = SSL_get_error(session->ssl_conn, 0);
        unsigned long ossl_err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(ossl_err, err_buf, sizeof(err_buf));
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                      "h3_session: SSL_accept_stream returned NULL, ssl_error=%d, ossl_err=%lu: %s, errno=%d (%s)",
                      err, ossl_err, err_buf, errno, strerror(errno));
    }

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: processed %d streams total", processed);

    /* Check if nghttp3 has data to send and write it out */
    nghttp3_vec vec[16];
    nghttp3_ssize sveccnt;
    int64_t stream_id;
    int fin;

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: checking for nghttp3 data to send");

    for (;;) {
        sveccnt = nghttp3_conn_writev_stream(session->ngh3, &stream_id, &fin, vec, 16);
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                      "h3_session: nghttp3_conn_writev_stream returned %ld", (long)sveccnt);

        if (sveccnt == 0) {
            /* No more data to send from nghttp3 */
            /* If GOAWAY was queued, it has now been written out */
            if (session->goaway_queued) {
                session->goaway_queued = 0;
                session->shutdown_requested = 1;
                ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                              "h3_session: GOAWAY written, will send CONNECTION_CLOSE");
            }
            break;
        }

        if (sveccnt < 0) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, session->s,
                          "h3_session: nghttp3_conn_writev_stream failed: %ld", (long)sveccnt);
            break;
        }

        /* nghttp3 has data to send on stream_id */
        const char *stream_type = (stream_id == -1) ? "CONTROL" :
                                  ((stream_id & 0x2) ? "UNI" : "BIDI");
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                      "h3_session: nghttp3 wants to send %ld vectors on stream %ld (%s), fin=%d",
                      (long)sveccnt, (long)stream_id, stream_type, fin);

        /* Find or create the SSL stream for this stream_id */
        SSL *write_stream = NULL;
        apr_uint64_t lookup_id = (apr_uint64_t)stream_id;
        h3_stream *h3s = apr_hash_get(session->streams, &lookup_id, sizeof(lookup_id));
        if (h3s && h3s->ssl_stream) {
            write_stream = h3s->ssl_stream;
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                          "h3_session: found existing SSL stream for stream %ld", (long)stream_id);
        } else {
            /* Need to create a new stream for sending */
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                          "h3_session: stream %ld not found in hash (h3s=%p), creating new stream",
                          (long)stream_id, (void*)h3s);
            write_stream = SSL_new_stream(session->ssl_conn, SSL_STREAM_FLAG_UNI);
            if (!write_stream) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, session->s,
                              "h3_session: failed to create stream for sending");
                break;
            }
            /* Set to non-blocking mode */
            SSL_set_blocking_mode(write_stream, 0);
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                          "h3_session: created new stream %lu for sending nghttp3 data",
                          (unsigned long)SSL_get_stream_id(write_stream));
        }

        /* Write all vectors to the stream */
        size_t total_written = 0;
        int write_success = 1;

        /* Verify and enforce non-blocking mode */
        int blocking = SSL_get_blocking_mode(write_stream);
        if (blocking) {
            ap_log_cerror(APLOG_MARK, APLOG_WARNING, 0, session->c,
                          "h3_session: stream %ld was in blocking mode, setting to non-blocking", (long)stream_id);
            SSL_set_blocking_mode(write_stream, 0);
        }

        for (nghttp3_ssize i = 0; i < sveccnt; i++) {
            size_t written = 0;
            uint64_t write_flags = 0;

            /* If this is the last vector and fin=1, use CONCLUDE flag to close stream */
            if (fin && i == sveccnt - 1) {
                write_flags = SSL_WRITE_FLAG_CONCLUDE;
                ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                              "h3_session: last vector with fin=1, will CONCLUDE stream %ld",
                              (long)stream_id);
            }

            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, session->s,
                          "h3_session: calling SSL_write_ex2 on stream %ld, vec[%ld].len=%zu, flags=%llu",
                          (long)stream_id, (long)i, (size_t)vec[i].len, (unsigned long long)write_flags);
            int write_ret = SSL_write_ex2(write_stream, vec[i].base, vec[i].len, write_flags, &written);
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, session->s,
                          "h3_session: SSL_write_ex2 returned ret=%d, written=%zu",
                          write_ret, (size_t)written);
            if (write_ret != 1 || written != vec[i].len) {
                int ssl_err = SSL_get_error(write_stream, write_ret);
                /* WANT_WRITE is normal for non-blocking - break and come back later */
                if (ssl_err == SSL_ERROR_WANT_WRITE || ssl_err == SSL_ERROR_WANT_READ) {
                    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, session->s,
                                  "h3_session: SSL_write_ex2 on stream %ld would block (ssl_err=%d), will retry later",
                                  (long)stream_id, ssl_err);
                } else {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, session->s,
                                  "h3_session: SSL_write_ex2 failed on stream %ld: ret=%d, ssl_err=%d",
                                  (long)stream_id, write_ret, ssl_err);
                }
                write_success = 0;
                break;
            }
            total_written += written;
        }

        /* Only tell nghttp3 we consumed the data if write succeeded */
        if (write_success && total_written > 0) {
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                          "h3_session: calling nghttp3_conn_add_write_offset(stream=%ld, total_written=%lu)",
                          (long)stream_id, (unsigned long)total_written);

            int rv = nghttp3_conn_add_write_offset(session->ngh3, stream_id, total_written);
            if (rv != 0) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, session->s,
                              "h3_session: nghttp3_conn_add_write_offset failed: %d", rv);
            } else {
                ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                              "h3_session: nghttp3_conn_add_write_offset SUCCESS");
            }

            /* If we sent with FIN (CONCLUDE), check if stream is now finished and close it */
            if (fin) {
                int write_state = SSL_get_stream_write_state(write_stream);
                ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                              "h3_session: stream %ld write_state=%d (FINISHED=%d)",
                              (long)stream_id, write_state, SSL_STREAM_STATE_FINISHED);

                if (write_state == SSL_STREAM_STATE_FINISHED) {
                    /* Stream write is finished, tell nghttp3 to close it */
                    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                                  "h3_session: closing stream %ld with nghttp3_conn_close_stream",
                                  (long)stream_id);
                    rv = nghttp3_conn_close_stream(session->ngh3, stream_id, NGHTTP3_H3_NO_ERROR);
                    if (rv != 0) {
                        ap_log_error(APLOG_MARK, APLOG_ERR, 0, session->s,
                                      "h3_session: nghttp3_conn_close_stream failed: %d", rv);
                    }

                    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                                  "h3_session: stream %ld write side closed with FIN",
                                  (long)stream_id);

                    /* Free the SSL stream object */
                    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                                  "h3_session: freeing SSL stream %ld",
                                  (long)stream_id);
                    SSL_free(write_stream);

                    /* Remove stream from session hash to prevent use-after-free */
                    apr_hash_set(session->streams, &stream_id, sizeof(stream_id), NULL);

                    /* DON'T call shutdown here - wait for on_stream_close callback
                     * which fires when BOTH sides are closed (we sent FIN, client sent FIN) */
                    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                                  "h3_session: stream %ld write side done, waiting for stream_close callback",
                                  (long)stream_id);
                }
            }
        }

        /* If write failed (including WANT_WRITE), we must break out of the write loop
         * because nghttp3_conn_writev_stream will keep returning the same data
         * until we call nghttp3_conn_add_write_offset (which we only do on success).
         * We'll retry on the next UDP event. */
        if (!write_success) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, session->s,
                          "h3_session: write blocked on stream %ld, returning to MPM", (long)stream_id);
            break;  /* Exit the write loop - will retry on next event */
        }
    }

    /* CRITICAL: Call SSL_handle_events after writing to flush data to UDP socket
     * Without this, the SSL_write_ex data stays buffered and never goes out */
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, session->s,
                  "h3_session: calling SSL_handle_events to flush writes to UDP");
    SSL_handle_events(session->ssl_conn);

    /* After GOAWAY is sent, let the client close the connection.
     * DON'T send CONNECTION_CLOSE - wait for client to do it.
     * This gives client time to send final ACKs without ERR_DRAINING. */
    if (session->shutdown_requested) {
        ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                      "h3_session: GOAWAY sent, waiting for client to close connection");

        session->shutdown_requested = 0;
        session->aborted = 1;  /* Mark session as done so we stop processing */
    }

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: processing session - END, returning %s",
                  (processed > 0) ? "SUCCESS" : "EAGAIN");

    /* Return EAGAIN if no streams were processed, SUCCESS otherwise */
    return (processed > 0) ? APR_SUCCESS : APR_EAGAIN;
}

void h3_session_destroy(h3_session *session)
{
    /* No thread to stop - Apache handles the UDP socket monitoring */

    if (session->ngh3) {
        nghttp3_conn_del(session->ngh3);
    }

    ap_log_error(APLOG_MARK, APLOG_INFO, 0, session->s,
                  "h3_session: destroyed session");
}
