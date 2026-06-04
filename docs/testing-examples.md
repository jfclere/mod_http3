# Example Programs

Standalone programs for testing and learning. All are built by default.

```sh
cmake -B build -DBUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
```

Binaries are in `build/bin/`.

## server (build/bin/server)

Minimal QUIC/HTTP/3 server. Serves files from the current working directory; returns a 20-byte body for unknown paths. Single-threaded, one connection at a time.

**Source:** `mod_http3/examples/server.c`

Generate a certificate and start:

```sh
bash scripts/mkcert.sh keys
./build/bin/server 4433 keys/server.crt keys/server.key &
```

Verify:

```sh
ss -ulnp | grep 4433
# UNCONN 0 0 0.0.0.0:4433 0.0.0.0:*
```

Test with the built-in client:

```sh
./build/bin/quic_client_test localhost 4433 keys/server.crt
```

Test with curl:

```sh
/opt/curl/bin/curl --http3 --cacert keys/server.crt -sI https://localhost:4433/
```

Test with Chrome:

```sh
google-chrome \
    --user-data-dir=/tmp/quic_dev \
    --ignore-certificate-errors \
    --origin-to-force-quic-on=localhost:4433 \
    https://localhost:4433
```

Stop:

```sh
pkill -f "build/bin/server"
```

**Behavior:**

| Request Path | Response |
|---|---|
| `/` or unknown | 20-byte default body (`12345678901234567890`) |
| `/filename` | Contents of `./filename` if it exists |
| `HEAD` method | Headers only, no body |
| `GET` method | Headers + body |

**Limitations:** Single-threaded, no HTTP/2 or HTTP/1.1 fallback, no Alt-Svc header, no TLS session resumption.

## quic_client_test (build/bin/quic_client_test)

Native QUIC/HTTP/3 client. Sends one or more concurrent GET requests. Verifies the server certificate against a supplied CA cert. No curl needed.

**Source:** `mod_http3/examples/quic_client_test.c`

Start a server first:

```sh
bash scripts/mkcert.sh keys
./build/bin/server 4433 keys/server.crt keys/server.key &
```

Single stream:

```sh
./build/bin/quic_client_test localhost 4433 keys/server.crt
```

Expected: `Status 200`, `All streams complete`.

Five concurrent streams:

```sh
./build/bin/quic_client_test localhost 4433 keys/server.crt 5
```

TLS handshake trace (writes `trace.txt`):

```sh
./build/bin/quic_client_test -trace localhost 4433 keys/server.crt
```

**Usage:**

```sh
./build/bin/quic_client_test [-trace] <hostname> <port> <cacert.pem> [num_streams]
```

| Argument | Description |
|---|---|
| `-trace` | Write TLS message trace to `trace.txt` |
| `hostname` | Server hostname (e.g., `localhost`) |
| `port` | Server UDP port (e.g., `4433`) |
| `cacert.pem` | CA certificate to verify the server |
| `num_streams` | Number of concurrent HTTP/3 GET streams (default: 1) |

**Limitations:** Sends GET requests only, requests root path `/` only, no HTTP/2 or HTTP/1.1 fallback.

## quic_server_test (build/bin/quic_server_test)

**DO NOT USE.** This test is broken. Both QUIC_TSERVER instances try to handshake with an external address that does not exist, so the test hangs indefinitely.

Use `build/bin/server` + `build/bin/quic_client_test` instead.

**Source:** `mod_http3/examples/quic_server_test.c`

## biomemexample (build/bin/biomemexample)

Demonstrates OpenSSL BIO memory buffers. `fork()`s into client and server processes that perform a TLS handshake over Unix pipes using memory BIOs (`BIO_s_mem`). Not QUIC or HTTP/3 related - an OpenSSL BIO learning example.

**Source:** `mod_http3/examples/biomemexample.c`

Run:

```sh
./build/bin/biomemexample
```

The client sends `"abcdefgh"`, the server sends `"ABCDEFGH"`. Both print what they sent and received.

## apr_miniserver (build/bin/apr_miniserver)

Minimal UDP server using APR socket types. Listens on UDP port 8081, spawns a thread per datagram, prints received data. Not QUIC or HTTP/3 related - an APR learning example.

**Source:** `mod_http3/examples/miniserver.c`

Run:

```sh
./build/bin/apr_miniserver &
```

Send a message:

```sh
echo "hello" > /dev/udp/localhost/8081
```

Server output:

```
from: <source port>
from: 127.0.0.1
from: hello
```

Stop:

```sh
pkill -f apr_miniserver
```
