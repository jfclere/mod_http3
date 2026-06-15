# Switch from Standalone to MPM Integration

## Summary

Disabled the old standalone QUIC event loop and switched to MPM integration where the MPM handles polling and accepts QUIC connections.

## Changes Made

### File: mod_http3/src/mod_http3.c

#### 1. Disabled Standalone Worker Thread (h3_child_init)

**Location:** Line ~838-881

**Before:**
```c
static void h3_child_init(apr_pool_t* pchild, server_rec* s)
{
    // Created worker thread
    // Worker thread ran server() which ran run_quic_server()
    // run_quic_server() had its own SSL_poll event loop
    apr_thread_create(&worker_thread, NULL, worker_thread_main, ...);
}
```

**After:**
```c
static void h3_child_init(apr_pool_t* pchild, server_rec* s)
{
    /* DISABLED: Now using MPM integration instead of standalone event loop */
    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                 "h3_child_init: QUIC listener registered with MPM");
    return;
    
    /* OLD STANDALONE CODE - DISABLED ... */
}
```

#### 2. Added MPM Integration (ALREADY DONE)

These were added earlier:

**h3_quic_accept()** - Custom accept function for MPM  
**h3_post_config()** - Registers QUIC listener with ap_listeners  
**h3_create_connection()** - Creates conn_rec for QUIC streams  

## Architecture Comparison

### OLD: Standalone Event Loop

```
Child Process
    ↓
h3_child_init()
    ↓
Creates Worker Thread
    ↓
worker_thread_main()
    ↓
server() in ossl-nghttp3.c
    ↓
run_quic_server()
    ↓
Infinite loop with SSL_poll()
    ↓
SSL_accept_connection()
SSL_accept_stream()
    ↓
create_connection() - creates h3_conn_rec_t
    ↓
Standalone request processing
```

**Problems:**
- Runs completely separate from MPM
- Doesn't integrate with Apache's listener model
- Worker thread in each child process
- Own event loop with SSL_poll

### NEW: MPM Integration

```
Apache Startup
    ↓
h3_post_config()
    ↓
Registers ap_listen_rec with:
  - UDP socket
  - accept_func = h3_quic_accept
  - Adds to ap_listeners chain
    ↓
MPM Child Process
    ↓
Polls TCP + UDP listeners
    ↓
UDP socket ready
    ↓
Calls lr->accept_func() → h3_quic_accept()
    ↓
SSL_accept_connection()
SSL_accept_stream()
    ↓
Returns h3_quic_conn_state_t
    ↓
MPM calls ap_run_create_connection()
    ↓
h3_create_connection() hook
    ↓
Creates conn_rec
Stores QUIC state
Adds I/O filters
    ↓
Standard Apache request processing
```

**Benefits:**
- Fully integrated with MPM
- No separate worker threads
- MPM handles all polling
- Works with prefork/worker/event/motorz
- Standard Apache request lifecycle

## What Happens Now

### When Apache Starts

1. **post_config phase:**
   - h3_post_config() runs
   - Creates UDP socket with SO_REUSEPORT
   - Creates SSL_CTX and SSL listener
   - Registers listener with ap_listeners
   - Sets accept_func = h3_quic_accept

2. **child_init phase:**
   - h3_child_init() runs
   - Logs "QUIC listener registered with MPM"
   - Returns immediately (no worker thread created)

### When Request Arrives

1. **UDP packet arrives on port 4433**

2. **MPM detects activity:**
   - apr_pollset_poll() returns
   - Identifies QUIC listener

3. **MPM calls accept:**
   - `lr->accept_func(&csd, lr, ptrans)`
   - Calls h3_quic_accept()

4. **h3_quic_accept() executes:**
   - SSL_handle_events() for timeouts
   - SSL_accept_connection() for new QUIC connections
   - SSL_accept_stream() for streams
   - Returns h3_quic_conn_state_t

5. **MPM creates connection:**
   - `ap_run_create_connection(ptrans, server, csd, ...)`
   - h3_create_connection() hook intercepts
   - Creates conn_rec
   - Stores QUIC state in conn_config
   - Adds H3_NET_IN and H3_NET_OUT filters
   - Returns conn_rec

6. **MPM processes connection:**
   - ap_process_connection(conn_rec, csd)
   - Standard Apache request processing
   - Filters handle I/O

## What Still Needs Work

### 1. I/O Filters Must Use QUIC Streams

Current filters (h3_filter_in/h3_filter_out) are stubs. They need to:

```c
static apr_status_t h3_filter_in(...)
{
    h3_quic_conn_state_t *qcs = ap_get_module_config(f->c->conn_config, &http3_module);
    
    if (qcs && qcs->ssl_stream) {
        /* QUIC from MPM - use SSL_read_ex */
        SSL_read_ex(qcs->ssl_stream, buffer, size, &bytes_read);
    } else {
        /* Old standalone code path - if any remains */
    }
}
```

### 2. HTTP/3 Protocol Handling

The protocol filters (h3_filter_in_proto/h3_filter_out_proto) need to:
- Parse HTTP/3 frames (using nghttp3)
- Populate request_rec from HEADERS frames
- Generate HTTP/3 response frames

### 3. Connection Cleanup

When stream/connection closes:
- SSL_free(ssl_stream)
- Remove from connections hash
- Proper shutdown

### 4. Peer Addresses

Extract from QUIC:
```c
// In h3_create_connection
c->client_addr = get_peer_addr_from_ssl(qcs->ssl_conn, ptrans);
c->local_addr = get_local_addr_from_ssl(qcs->ssl_conn, ptrans);
```

## Testing

### Rebuild

```bash
cd /home/jfclere/TMP/mod_http3
cmake --build build
cmake --install build
```

### Check Logs

```bash
# Start Apache
apachectl -k start

# Check error log
tail -f /path/to/error_log

# Should see:
# h3_post_config: ... Registering QUIC listener on port 4433
# mod_http3: QUIC listener registered successfully on port 4433
# h3_child_init: QUIC listener registered with MPM

# Should NOT see:
# run_quic_server started!
# worker_thread_main
```

### Verify Listener

```bash
# Check UDP socket
netstat -tuln | grep 4433
# Should see UDP listening

# Check process
ps aux | grep httpd
# Should NOT see extra worker threads
```

### Test Connection

```bash
curl --http3 https://localhost:4433/
```

**Expected:**
- QUIC connection accepted
- h3_quic_accept called
- h3_create_connection called
- Request processing begins
- **May fail at I/O filter stage** (needs implementation)

## Benefits of This Change

1. **Cleaner architecture** - One event loop (MPM's)
2. **Less resource usage** - No extra worker threads
3. **Better integration** - Standard Apache hooks
4. **Multi-MPM support** - Works with all MPMs
5. **Easier debugging** - Standard Apache code paths

## Next Steps

1. Implement I/O filters to use SSL_read_ex/SSL_write_ex
2. Add peer address extraction
3. Test with actual QUIC clients
4. Performance tuning
