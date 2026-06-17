# HTTP/3 Wakeup Mechanism Debugging Session Summary

## Problem Statement

Dummy TCP socket created in parent process (h3_post_config) is inherited by all children. When background thread connects to wake Apache, **random child accepts** instead of the child that has the ready HTTP/3 request.

**Architecture:**
- Background thread per child process handles QUIC via SSL_poll
- Thread queues ready HTTP/3 requests in `session->ready_streams`
- Thread needs to wake Apache MPM to call `h3_quic_accept` to dequeue requests
- Current: Single dummy TCP socket shared by all children → wrong child wakes up

---

## Attempts and Outcomes

### **Attempt 1: Create dummy socket in h3_child_init**

**Approach:** Each child creates its own unique dummy socket port in child_init hook instead of parent

**Implementation:**
```c
// In h3_child_init (after fork, per child):
apr_socket_create(&dummy_sock, ...);
apr_socket_bind(dummy_sock, ...);  // port 0 = random port
apr_socket_listen(dummy_sock, 128);
// Add to ap_listeners
```

**Outcome:** ❌ **FAILED**

**Reason:** MPM doesn't poll sockets added to ap_listeners after child_init runs. The MPM has already finalized its listener list and set up poll structures before child_init executes. h3_quic_accept was never called even though connections existed.

---

### **Attempt 2: Thread flushes responses every loop iteration**

**Approach:** Call h3_session_process() in thread loop to send queued responses without any wakeup mechanism

**Implementation:**
```c
// In quic_event_thread main loop:
while (thread_running) {
    SSL_poll(...);
    // Process events
    
    // Always flush all sessions
    for (all sessions) {
        h3_session_process(session, NULL);  // Calls nghttp3_conn_writev_stream
    }
}
```

**Outcome:** ⚠️ **PARTIAL SUCCESS**

**Issues Found:**
1. **Timing race:** Response queued at 35.255495ms, but thread last processed at 35.255035ms (460μs gap)
2. **Child exits:** Apache processes request → queues response → child exits before next thread loop
3. Thread logs showed: "nghttp3_conn_writev_stream returned 0" (no data) then response queued later

**Why it fails:** Apache thread and background thread aren't synchronized. Response gets queued AFTER thread checks for data.

---

### **Attempt 3: Multiple dummy sockets (one per child slot)**

**Approach:** 
1. Create N dummy sockets in h3_post_config (N=10 for server limit)
2. Each child determines its slot number via `ap_find_child_by_pid()`
3. Each child closes unwanted sockets, keeps only its assigned port
4. Thread connects to its own child's port → only that child wakes up

**Implementation:**
```c
// h3_post_config:
for (i = 0; i < 10; i++) {
    create dummy_sock[i] on random port
    add to ap_listeners
    store port in conf->wakeup_addrs[i]
}

// h3_child_init:
my_slot = ap_find_child_by_pid(getpid());
for (all dummy sockets) {
    if (socket_index != my_slot) {
        apr_socket_close(lr->sd);  // Close unwanted sockets
    }
}
```

**Outcome:** ❌ **FAILED**

**Reason:** Closing sockets with `apr_socket_close()` in child_init closed file descriptors that other children needed. All children inherited the SAME file descriptor numbers from parent (e.g., fd=21 for dummy socket #1). When child A closes fd=21, child B (which needs that fd for ITS dummy socket) can't use it anymore.

**Error logs:** "Bad file descriptor: quic_event_thread: select failed"

---

### **Attempt 4: Remove unwanted dummy sockets from ap_listeners in child_init**

**Approach:** Instead of closing sockets, surgically remove unwanted listeners from the ap_listeners linked list

**Implementation:**
```c
// h3_child_init:
ap_listen_rec **ptr = &ap_listeners;
while (*ptr) {
    if (is_dummy_socket && not_my_slot) {
        *ptr = (*ptr)->next;  // Remove from linked list
    }
}
```

**Outcome:** ❌ **CRASHED**

**Error:** Segmentation fault - child processes crashing

**Reason:** Apache MPM has already set up internal poll structures based on parent's ap_listeners before child_init runs. Modifying the linked list after fork causes memory corruption or invalid pointer access when MPM tries to poll.

---

### **Attempt 5: Keep all sockets, check port in h3_quic_accept**

**Approach:**
- All children poll all dummy sockets (no removal)
- In h3_quic_accept, check if `lr->bind_addr->port` matches `my_assigned_port`
- Return `APR_EAGAIN` without accepting if it's not my port

**Implementation:**
```c
static apr_status_t h3_quic_accept(...) {
    if (lr->bind_addr->port != ql->wakeup_connect_addr->port) {
        // Not my port
        return APR_EAGAIN;
    }
    // My port - accept it
}
```

**Outcome:** ❌ **FAILED**

**Reason:** Infinite loop. If we return APR_EAGAIN without accepting, the connection stays in the listen queue → socket remains readable → MPM immediately calls h3_quic_accept again → infinite loop. Logs showed thousands of "h3_quic_accept: CALLED!" messages per second, CPU spinning.

---

### **Attempt 6: Accept and close if wrong port**

**Approach:** All children attempt accept(), check port after accepting, close connection if wrong port

**Implementation:**
```c
apr_socket_accept(&conn, lr->sd, ptrans);
if (port != my_port) {
    apr_socket_close(conn);
    return APR_EAGAIN;
}
```

**Outcome:** ❌ **FAILED**

**Reason:** Race condition. `accept()` is atomic - only ONE child succeeds. But it might be the WRONG child. Wrong child accepts and discards; right child never wakes up because connection is gone.

---

### **Attempt 7: Make connect() blocking instead of non-blocking**

**Approach:** Change thread's socket from `SOCK_NONBLOCK` to blocking to ensure connect() completes

**Original code:**
```c
int wake_sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
connect(wake_sock, ...);  // Returns EINPROGRESS
// Socket goes out of scope - connection never completes!
```

**Fix attempt:**
```c
int wake_sock = socket(AF_INET, SOCK_STREAM, 0);  // BLOCKING
connect(wake_sock, ...);  // Waits for completion
```

**Outcome:** ❌ **BROKE SYSTEM**

**Result:** No QUIC connections established at all. Thread behavior changed in unexpected ways. SSL_poll showed `result_count=0` continuously - no incoming connections accepted.

---

### **Attempt 8: Revert to simple single dummy socket**

**Approach:** Remove all multi-socket complexity, go back to original one shared dummy socket approach

**Changes:** Reverted h3_server_conf, h3_post_config, h3_child_init to simple single-socket version

**Outcome:** ❌ **STILL BROKEN**

**Result:** Even basic QUIC connections stopped working. `result_count=0`, no sessions created. System fundamentally broken by accumulated changes throughout the session.

---

### **Attempt 9: Git revert to earlier commit (608a6ea)**

**Approach:** `git stash && git checkout 608a6ea` - return to "Try with a dummy socket" commit

**Outcome:** ❌ **STILL BROKEN**

**Result:** Even old code doesn't work anymore. curl times out, no HTTP/3 responses. Environment or httpd state appears corrupted from repeated restarts and crashes.

---

## Key Technical Discoveries

### 1. **Shared Listen Queue Problem**
When dummy socket is created in parent process:
- Parent: `bind()` + `listen()` creates ONE listen queue in kernel
- Fork: All children inherit same file descriptor
- Kernel: Uses ONE shared queue for all processes
- Result: `accept()` call from ANY child can succeed - kernel picks randomly

### 2. **Apache MPM Lifecycle Timing**
```
Parent Process:
  1. pre_config hooks
  2. post_config hooks  ← Listeners MUST be added here
  3. fork() children
  
Child Process:
  4. child_init hooks   ← Too late to add listeners
  5. MPM poll loop starts
```
- MPM finalizes listener list and poll structures BEFORE child_init
- Adding to ap_listeners in child_init → ignored
- Modifying ap_listeners in child_init → crash

### 3. **File Descriptor Inheritance**
After fork, children share FD numbers:
```
Parent creates: fd=21 (dummy socket #1), fd=22 (dummy socket #2)
Child A inherits: fd=21, fd=22
Child B inherits: fd=21, fd=22 (SAME numbers)

Child A: close(fd=21)  → Closes dummy socket #1 FOR EVERYONE
Child B: tries to use fd=21 → "Bad file descriptor"
```

### 4. **Non-blocking Connect Leak**
```c
int wake_sock = socket(..., SOCK_NONBLOCK, ...);
connect(wake_sock, ...);  // Returns -1, errno=EINPROGRESS
// wake_sock goes out of scope
// Connection never completes - no FD kept alive!
```
The thread logged "connected to dummy socket port X" but `netstat` showed no connections - they were never actually established.

### 5. **Thread/Child Process Lifecycle Race**
```
Timeline:
35.255035 - Thread processes session, calls nghttp3_conn_writev_stream → 0 vectors
35.255153 - Thread connects to dummy socket (wakeup)
35.255495 - Apache queues response via nghttp3_conn_submit_response
35.255xxx - Child process exits

Response queued AFTER thread checked for data and BEFORE next loop iteration.
```

### 6. **Prefork MPM Does Call accept_func**
Verified in `/server/mpm/prefork/prefork.c`:
```c
Line 557: if (!lr->accept_func) lr->accept_func = ap_unixd_accept;
Line 666: status = lr->accept_func(&csd, lr, ptrans);
```
So the callback mechanism DOES work - our dummy sockets just weren't being polled for other reasons.

---

## Root Causes Analysis

### Why Per-Child Dummy Sockets Don't Work:

1. **Can't create in child_init** - MPM won't poll them
2. **Can't create in post_config then modify** - Causes crashes or FD conflicts  
3. **Can't filter in h3_quic_accept** - Creates infinite loops or race conditions
4. **Can't close unwanted sockets** - Breaks other children's FDs

### Why Single Shared Dummy Socket Has Issues:

1. **Shared listen queue** - Random child accepts the wakeup connection
2. **Wrong child wakes** - Child without ready request wastes time
3. **Right child sleeps** - Child with ready request never processes it
4. **Responses lost** - Queued after thread checks or before next loop

---

## Architectural Conclusion

**Per-child dummy sockets are fundamentally incompatible with Apache's MPM architecture.**

The root problem is the **mismatch between when listeners must be registered (post_config, in parent) and when children can determine their identity (child_init, after fork)**.

### Recommended Alternative Approaches:

1. **Thread Synchronization (mutex/condition variables)**
   ```
   Apache thread:
     - Queue response via nghttp3_conn_submit_response()
     - Signal condition variable: response_queued
     - Wait on condition variable: response_sent
     
   Background thread:
     - Wait on: response_queued
     - Call h3_session_process() to send via SSL_write
     - Signal: response_sent
   ```
   **Pros:** No dummy sockets, precise synchronization, right thread sends
   **Cons:** Blocks Apache thread (but so does TCP send anyway)

2. **Accept Shared Wakeups + Check in h3_quic_accept**
   ```
   - Keep single dummy socket (all children wake)
   - In h3_quic_accept: check child_sessions for ready requests
   - Children without requests: accept+close, return APR_EAGAIN
   - Child with request: accept, process request
   ```
   **Pros:** Simple, uses existing mechanism
   **Cons:** Wastes CPU on wrong children waking up

3. **Signals for Wakeup**
   ```
   - Thread sends signal to its own PID
   - Install signal handler in child
   - Handler marks flag, MPM wakes up
   ```
   **Pros:** OS-level targeting, no shared sockets
   **Cons:** Signal handling complexity, EINTR everywhere

4. **eventfd or pipe per child**
   ```
   - Each child creates eventfd in child_init
   - Add to ap_listeners somehow (but we know this fails...)
   - Thread writes to eventfd to wake
   ```
   **Cons:** Same timing problem as dummy sockets

---

## Files Modified During Session

- `/home/jfclere/TMP/mod_http3/mod_http3/src/mod_http3.c` - Main implementation
- Final state: Reverted to commit 608a6ea but system still broken

## Session Metrics

- **Total attempts:** 9 major approaches
- **Token usage:** ~139k / 200k
- **Final state:** Non-functional (even reverted code doesn't work)
- **Recommendation:** Fresh start with clear objective and clean environment

---

## Next Steps

1. **Clean restart:** Fresh httpd install, clear all state
2. **Pick ONE approach:** Likely mutex/condition variables (avoid dummy sockets entirely)
3. **Incremental testing:** Test each small change before building further
4. **Commit frequently:** So we can revert to known-working states
