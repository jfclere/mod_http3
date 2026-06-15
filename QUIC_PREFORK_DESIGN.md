# QUIC Integration with Prefork MPM Design

## Overview

This design explores integrating QUIC/HTTP3 support into Apache httpd's prefork MPM using the existing `accept_function` callback mechanism in `ap_listen_rec`.

## Key Insight

Prefork MPM already supports pluggable accept functions:
```c
typedef apr_status_t (*accept_function)(void **csd, ap_listen_rec *lr, apr_pool_t *ptrans);
```

Each listener can have a custom accept function. TCP uses `ap_unixd_accept()`, we can add `ap_quic_accept()`.

## Architecture

### Multi-Process Model with SO_REUSEPORT

```
                    Incoming UDP packets on port 443
                                 |
                    Kernel (SO_REUSEPORT distribution)
                                 |
        ┌────────────────────────┼────────────────────────┐
        |                        |                        |
    Child 1                  Child 2                  Child 3
        |                        |                        |
   UDP Socket FD:3         UDP Socket FD:3         UDP Socket FD:3
        |                        |                        |
   SSL *listener           SSL *listener           SSL *listener
        |                        |                        |
   Prefork event           Prefork event           Prefork event
   loop polls UDP          loop polls UDP          loop polls UDP
        |                        |                        |
   Calls                   Calls                   Calls
   ap_quic_accept()       ap_quic_accept()       ap_quic_accept()
        |                        |                        |
   Returns conn_rec        Returns conn_rec        Returns conn_rec
   with QUIC stream        with QUIC stream        with QUIC stream
```

### Data Structures

```c
/* QUIC-specific listener data */
typedef struct quic_listener_t {
    SSL_CTX *ssl_ctx;           // OpenSSL QUIC context
    SSL *ssl_listener;          // OpenSSL listener object  
    apr_pool_t *pool;           // Pool for this listener
    server_rec *server;         // Server config
    const char *cert_path;
    const char *key_path;
} quic_listener_t;

/* QUIC connection state (stored in conn_rec->conn_config) */
typedef struct quic_conn_state_t {
    SSL *ssl_conn;              // QUIC connection object
    SSL *ssl_stream;            // Current QUIC stream
    apr_uint64_t stream_id;     // Stream ID
    nghttp3_conn *h3_conn;      // HTTP/3 connection (if HTTP/3)
} quic_conn_state_t;
```

### Integration Points

#### 1. Listener Registration (pre_config hook)

mod_http3 registers QUIC listeners in the pre_config hook:

```c
static int quic_pre_config(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp)
{
    // For each configured QUIC listener:
    // 1. Create UDP socket with SO_REUSEPORT
    // 2. Create OpenSSL QUIC listener
    // 3. Add to ap_listeners chain with ap_quic_accept callback
    
    return OK;
}
```

#### 2. Accept Function (called by prefork MPM)

```c
apr_status_t ap_quic_accept(void **accepted, ap_listen_rec *lr, apr_pool_t *ptrans)
{
    quic_listener_t *ql = lr->protocol_data;
    
    // Call SSL_handle_events to process timeouts
    SSL_handle_events(ql->ssl_listener);
    
    // Accept QUIC connections
    SSL *conn;
    while ((conn = SSL_accept_connection(ql->ssl_listener, 
                                         SSL_ACCEPT_STREAM_NO_BLOCK))) {
        // Accept a stream from this connection
        SSL *stream = SSL_accept_stream(conn, SSL_ACCEPT_STREAM_NO_BLOCK);
        if (stream) {
            // Create conn_rec with QUIC stream
            *accepted = create_quic_conn_rec(stream, conn, lr, ptrans);
            return APR_SUCCESS;
        }
    }
    
    return APR_EAGAIN;  // No streams ready
}
```

#### 3. Connection Record Creation

```c
static void* create_quic_conn_rec(SSL *stream, SSL *conn, 
                                   ap_listen_rec *lr, apr_pool_t *ptrans)
{
    conn_rec *c = ap_new_connection(ptrans, lr->server, 
                                     /* fake socket */ NULL, 
                                     lr->bind_addr, lr->bind_addr, 
                                     lr->sbh, ptrans);
    
    // Store QUIC state
    quic_conn_state_t *qcs = apr_pcalloc(ptrans, sizeof(*qcs));
    qcs->ssl_conn = conn;
    qcs->ssl_stream = stream;
    qcs->stream_id = SSL_get_stream_id(stream);
    
    ap_set_module_config(c->conn_config, &http3_module, qcs);
    
    // Install QUIC I/O filters
    ap_add_input_filter("QUIC_IN", qcs, NULL, c);
    ap_add_output_filter("QUIC_OUT", qcs, NULL, c);
    
    return c;
}
```

#### 4. I/O Filters

Replace socket I/O with SSL_read_ex/SSL_write_ex:

```c
static apr_status_t quic_input_filter(ap_filter_t *f, 
                                       apr_bucket_brigade *bb,
                                       ap_input_mode_t mode, 
                                       apr_read_type_e block,
                                       apr_off_t readbytes)
{
    quic_conn_state_t *qcs = f->ctx;
    char buf[8192];
    size_t bytes_read;
    
    int ret = SSL_read_ex(qcs->ssl_stream, buf, sizeof(buf), &bytes_read);
    
    if (ret > 0) {
        apr_bucket *b = apr_bucket_heap_create(buf, bytes_read, 
                                                 NULL, bb->bucket_alloc);
        APR_BRIGADE_INSERT_TAIL(bb, b);
        return APR_SUCCESS;
    }
    
    int ssl_err = SSL_get_error(qcs->ssl_stream, ret);
    if (ssl_err == SSL_ERROR_WANT_READ) {
        return APR_EAGAIN;
    }
    
    return APR_EOF;
}
```

### Flow Through Prefork MPM

1. **Child starts**: Prefork child_main() initializes
2. **Pollset setup**: Child adds TCP + QUIC listeners to pollset
3. **Event loop**: apr_pollset_poll() waits for activity
4. **UDP socket ready**: Pollset returns QUIC listener
5. **Accept callback**: Prefork calls `ap_quic_accept()`
6. **OpenSSL demux**: Callback uses SSL_accept_connection/stream
7. **conn_rec created**: Returns Apache connection with QUIC state
8. **Request processing**: Standard Apache request processing pipeline
9. **I/O filters**: QUIC filters handle SSL_read_ex/SSL_write_ex

## Advantages

1. **Reuse prefork infrastructure**
   - Process management
   - Scoreboard
   - Graceful restart
   - Connection lifecycle

2. **No changes to prefork MPM code**
   - Uses existing accept_function callback
   - Prefork doesn't know about QUIC

3. **Standard Apache request processing**
   - All existing modules work
   - Logging, auth, handlers all work
   - No special-casing

4. **SO_REUSEPORT benefits**
   - Kernel load balancing
   - Process isolation
   - Connection affinity

## Disadvantages

1. **Doesn't leverage QUIC multiplexing**
   - Prefork is one-connection-per-process
   - Can't handle multiple streams in parallel
   - Wastes QUIC's main benefit

2. **Resource inefficient**
   - Need many child processes
   - One QUIC stream ties up whole process

3. **Connection state complexity**
   - QUIC connection shared across multiple accept() calls
   - Need to track which streams came from which connections

## Better Alternative: Event MPM

Event MPM would be a better target:
- Worker thread pool
- Can handle multiple streams in parallel
- Already has async I/O model
- Better resource utilization

But prefork is simpler to prototype with!

## Implementation Phases

### Phase 1: Minimal Prototype
- Single QUIC listener
- Basic accept callback
- Fake socket in conn_rec
- Manual testing

### Phase 2: I/O Filters
- Implement QUIC input filter
- Implement QUIC output filter
- Handle SSL_ERROR_WANT_READ/WRITE

### Phase 3: HTTP/3 Integration
- Add nghttp3 parsing
- Populate request_rec from HEADERS frames
- Generate HTTP/3 response frames

### Phase 4: Production Hardening
- Timeout handling
- Error recovery
- Graceful shutdown
- Multiple listeners

## Open Questions

1. **How to handle QUIC connections that spawn multiple streams?**
   - Each stream becomes separate accept()?
   - Need to cache SSL *conn somewhere
   
2. **How to integrate with existing h3_filter code?**
   - Reuse from current mod_http3
   - Or redesign as true Apache filters?

3. **What about the UDP socket itself?**
   - Store in apr_socket_t wrapper?
   - Or keep it separate in quic_listener_t?

4. **Timeout handling?**
   - SSL_get_event_timeout() in accept callback?
   - How to wake prefork when timeout expires?
