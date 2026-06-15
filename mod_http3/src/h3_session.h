/*
 * h3_session.h - HTTP/3 session management (similar to h2_session)
 */

#ifndef __H3_SESSION_H__
#define __H3_SESSION_H__

#include <httpd.h>
#include <openssl/ssl.h>
#include <nghttp3/nghttp3.h>

/* Forward declarations */
typedef struct h3_session h3_session;
typedef struct h3_stream h3_stream;

/* HTTP/3 session - manages all streams on a QUIC connection */
struct h3_session {
    conn_rec *c;                    /* Apache connection (may be NULL if no conn_rec yet) */
    server_rec *s;                  /* Server config */
    apr_pool_t *pool;               /* Session pool */

    SSL *ssl_listener;              /* QUIC listener (for accepting streams) */
    SSL *ssl_conn;                  /* QUIC connection */
    nghttp3_conn *ngh3;             /* nghttp3 connection */

    apr_hash_t *streams;            /* Stream ID -> h3_stream */
    int aborted;                    /* Session aborted */
    int goaway_queued;              /* nghttp3_conn_shutdown was called - GOAWAY queued */
    int shutdown_requested;         /* GOAWAY sent, now send CONNECTION_CLOSE */

    /* Queue of streams with complete HTTP requests ready to process */
    apr_array_header_t *ready_streams;  /* Array of h3_stream* with complete requests */

    /* Bridge thread for UDP socket monitoring */
    apr_thread_t *monitor_thread;   /* Thread that monitors UDP socket */
    int udp_fd;                     /* UDP socket file descriptor from OpenSSL */
    int wakeup_pipe[2];             /* Pipe to wake up Apache: [0]=read, [1]=write */
    apr_thread_mutex_t *mutex;      /* Protect shared state */
    int monitor_running;            /* Flag for thread lifecycle */
};

/* HTTP/3 stream - represents one request/response */
struct h3_stream {
    h3_session *session;
    apr_uint64_t stream_id;
    SSL *ssl_stream;                /* QUIC stream */

    request_rec *r;                 /* Apache request (for bidirectional streams) */
    int is_bidi;                    /* Bidirectional (request) vs unidirectional (control) */

    /* Request parsing state */
    int headers_complete;           /* All headers received */
    int request_complete;           /* Headers + body complete, ready to process */

    /* Parsed HTTP/3 pseudo-headers (stored until we can create request_rec) */
    const char *method;
    const char *scheme;
    const char *authority;
    const char *path;
    apr_table_t *headers;           /* Regular headers */

    /* Response body data for nghttp3 data reader */
    const uint8_t *response_data;
    size_t response_len;
    size_t response_offset;
};

/* Create a new HTTP/3 session */
apr_status_t h3_session_create(h3_session **psession,
                                server_rec *s,
                                SSL *ssl_listener,
                                SSL *ssl_conn,
                                apr_pool_t *pool);

/* Process the session - handle all streams */
apr_status_t h3_session_process(h3_session *session);

/* Destroy session */
void h3_session_destroy(h3_session *session);

#endif /* __H3_SESSION_H__ */
