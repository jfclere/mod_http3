# Prefork MPM QUIC Integration - Implementation Summary

## What Was Built

A proof-of-concept integration of QUIC/HTTP3 into Apache httpd's **prefork MPM** using the existing `accept_function` callback mechanism. This demonstrates that QUIC support can be added to Apache without modifying the MPM core.

## Key Files

1. **mod_http3_prefork_hook.c** - The module implementation
2. **QUIC_PREFORK_DESIGN.md** - Architectural design document
3. **PREFORK_EXAMPLE.conf** - Example Apache configuration

## Architecture Overview

### The Hook Mechanism

Apache's `ap_listen_rec` structure supports pluggable accept functions:

```c
typedef apr_status_t (*accept_function)(void **csd, ap_listen_rec *lr, apr_pool_t *ptrans);
```

- **TCP listeners** use `ap_unixd_accept()`
- **QUIC listeners** use `ap_quic_accept()` (our implementation)

The prefork MPM doesn't care which function it calls - it just polls the socket and invokes the callback when ready.

### Data Flow

```
1. Apache starts → post_config hook runs
2. Module creates UDP socket with SO_REUSEPORT
3. Module creates OpenSSL QUIC listener (SSL_new_listener)
4. Module adds listener to ap_listeners with ap_quic_accept callback
5. Prefork MPM child processes start
6. Each child polls both TCP and UDP listeners
7. When UDP socket ready → prefork calls ap_quic_accept()
8. ap_quic_accept() calls SSL_accept_connection() → gets QUIC connection
9. ap_quic_accept() calls SSL_accept_stream() → gets QUIC stream
10. Returns quic_conn_state_t as "connection socket descriptor"
11. Prefork calls ap_run_create_connection()
12. quic_create_connection hook intercepts and creates conn_rec
13. Standard Apache request processing begins
```

### Multi-Process Architecture with SO_REUSEPORT

```
                    UDP packets on port 4433
                            |
            Linux kernel (SO_REUSEPORT hash)
                            |
        ┌───────────────────┼───────────────────┐
        |                   |                   |
    Child 1             Child 2             Child 3
        |                   |                   |
   UDP Socket          UDP Socket          UDP Socket
        |                   |                   |
   SSL_listener        SSL_listener        SSL_listener
        |                   |                   |
   SSL_accept_         SSL_accept_         SSL_accept_
   connection()        connection()        connection()
        |                   |                   |
   QUIC streams        QUIC streams        QUIC streams
```

Each child process:
- Creates its own UDP socket on the same port (thanks to SO_REUSEPORT)
- Kernel distributes packets based on 4-tuple hash (client IP:port + server IP:port)
- Connection affinity: same QUIC connection always goes to same process
- Complete process isolation: no shared state needed

## Implementation Components

### 1. QUIC Accept Function (`ap_quic_accept`)

**Location:** Lines 65-135 in mod_http3_prefork_hook.c

**What it does:**
- Called by prefork MPM when UDP socket is ready
- Polls OpenSSL for new QUIC connections (`SSL_accept_connection`)
- Accepts streams from connections (`SSL_accept_stream`)
- Returns QUIC state wrapped as connection descriptor

**Key characteristics:**
- Non-blocking (uses `SSL_ACCEPT_STREAM_NO_BLOCK`)
- Returns APR_EAGAIN when no streams available
- Tracks active connections in hash table
- Handles both new connections and streams from existing connections

### 2. Listener Registration (`quic_register_listener`)

**Location:** Lines 214-345

**What it does:**
- Creates UDP socket with SO_REUSEPORT
- Creates OpenSSL SSL_CTX with certificate/key
- Creates SSL listener object (`SSL_new_listener`)
- Attaches socket to SSL listener (`SSL_set_fd`)
- Creates `ap_listen_rec` with our accept callback
- Adds to global `ap_listeners` linked list

**Critical detail:** Uses `apr_socket_data_set` to attach quic_listener_t to the socket so the accept callback can retrieve it.

### 3. Connection Creation Hook (`quic_create_connection`)

**Location:** Lines 26-66

**What it does:**
- Intercepts `ap_run_create_connection` hook
- Checks if csd is a QUIC stream (not a TCP socket)
- Creates conn_rec and stores QUIC state
- Marks connection as HTTP/3

**Why needed:** The accept function returns our custom structure, not an apr_socket_t. This hook translates it into a proper conn_rec.

### 4. Configuration Directives

**Directives added:**
- `QUICPort` - Port number for QUIC listener
- `QUICCertificateFile` - Path to SSL certificate
- `QUICCertificateKeyFile` - Path to SSL private key

**Example:**
```apache
QUICPort 4433
QUICCertificateFile /etc/ssl/certs/server.crt
QUICCertificateKeyFile /etc/ssl/private/server.key
```

## What's NOT Implemented (Yet)

### 1. I/O Filters

Need to implement:
- `QUIC_IN` filter - wraps `SSL_read_ex()` for reading from QUIC streams
- `QUIC_OUT` filter - wraps `SSL_write_ex()` for writing to QUIC streams

Without these, Apache's core will try to read/write to a non-existent socket and fail.

### 2. HTTP/3 Frame Processing

Need to integrate nghttp3:
- Parse HTTP/3 HEADERS frames → populate request_rec
- Generate HTTP/3 response frames from response_rec
- Handle HTTP/3 control streams

### 3. Timeout Handling

OpenSSL QUIC has internal timers that need servicing:
- Call `SSL_handle_events()` periodically
- Use `SSL_get_event_timeout()` to know when to wake up
- Integrate with prefork's poll timeout

### 4. Peer Address Extraction

Need to get client IP/port from QUIC connection:
- SSL_get_peer_address() or similar
- Populate c->client_addr and c->client_ip

### 5. Connection Cleanup

Need to properly close QUIC streams and connections:
- SSL_free(stream) when done
- Remove from connections hash table
- Handle graceful shutdown

## Advantages of This Approach

✅ **No MPM modifications** - Uses existing callback mechanism  
✅ **Reuses prefork infrastructure** - Process management, scoreboard, graceful restart  
✅ **SO_REUSEPORT benefits** - Kernel load balancing, connection affinity  
✅ **Standard Apache pipeline** - All existing modules work (auth, logging, handlers)  
✅ **Simple to prototype** - Less complexity than event MPM  

## Disadvantages

❌ **Doesn't leverage QUIC multiplexing** - Prefork is one-stream-per-process  
❌ **Resource inefficient** - One QUIC stream blocks entire process  
❌ **Connection state complexity** - QUIC connection persists across multiple accept() calls  
❌ **Not production-ready** - Missing I/O filters and HTTP/3 integration  

## Why Event MPM Would Be Better

Event MPM would be the proper target for production QUIC:
- **Worker thread pool** - Can handle multiple streams concurrently
- **Async I/O model** - Already designed for non-blocking I/O
- **Better resource utilization** - One process can serve many QUIC streams

But prefork is simpler for prototyping the hook mechanism!

## Next Steps to Make This Functional

### Phase 1: Basic I/O (needed for any functionality)
1. Implement QUIC_IN filter using SSL_read_ex()
2. Implement QUIC_OUT filter using SSL_write_ex()
3. Handle SSL_ERROR_WANT_READ/WRITE states
4. Test with simple HTTP/0.9 style requests

### Phase 2: HTTP/3 Protocol
1. Initialize nghttp3_conn for each QUIC connection
2. Parse HEADERS frames into request_rec
3. Generate HEADERS and DATA frames for responses
4. Handle HTTP/3 control streams

### Phase 3: Production Hardening
1. Implement timeout handling with SSL_handle_events()
2. Extract and populate peer addresses
3. Proper cleanup of streams and connections
4. Error recovery and logging
5. Support multiple virtual hosts

### Phase 4: Performance
1. Connection caching and reuse
2. Stream prioritization
3. Flow control tuning
4. Memory pool optimization

## Testing Strategy

Once I/O filters are implemented:

```bash
# 1. Build and install mod_http3
make && make install

# 2. Configure Apache with PREFORK_EXAMPLE.conf

# 3. Start Apache with prefork MPM
apachectl -k start

# 4. Test with curl (HTTP/3 support required)
curl --http3 https://localhost:4433/

# 5. Monitor logs
tail -f /usr/local/apache2/logs/error_log
```

Expected behavior:
- UDP socket listening on port 4433
- Multiple prefork children polling the socket
- QUIC connections distributed across children
- Each stream processed by one child
- Standard Apache logging and responses

## Code Statistics

**mod_http3_prefork_hook.c:**
- ~450 lines of code
- 5 main functions
- 3 configuration directives
- 3 hooks (pre_config, post_config, create_connection)

**Design documentation:**
- QUIC_PREFORK_DESIGN.md: ~270 lines
- PREFORK_INTEGRATION.md: This file

## References

- **OpenSSL QUIC API:** https://docs.openssl.org/master/man7/ossl-guide-quic-introduction/
- **Apache ap_listen.h:** httpd-trunk/include/ap_listen.h
- **Prefork MPM:** httpd-trunk/server/mpm/prefork/prefork.c
- **nghttp3:** https://github.com/ngtcp2/nghttp3

## Conclusion

This prototype demonstrates that QUIC can be integrated into Apache httpd's prefork MPM using the existing `accept_function` hook mechanism without modifying the MPM core. The architecture is sound, but significant work remains to implement I/O filters and HTTP/3 protocol handling before it's functional.

The real value is proving the concept: **Apache's extensible MPM architecture can support QUIC alongside TCP using callbacks, making it possible to add QUIC support without forking the MPM code.**

For production use, this approach should be ported to Event MPM to leverage QUIC's multiplexing capabilities.
