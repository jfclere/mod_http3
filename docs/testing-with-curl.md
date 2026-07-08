# HTTP/3 Testing with curl

For building curl with HTTP/3, see [the official curl HTTP/3 documentation](https://curl.se/docs/http3.html).

## Verify HTTP/3 Support

```sh
curl -V
# Look for: Features: ... HTTP3 ...
```

## Testing Commands

### HTTP/3 GET

```sh
curl --http3 -k -sI https://localhost:4433/
```

Expected response:

```http
HTTP/3 200
alt-svc: h3=":4433"; ma=60; persist=1
```

### Verbose Handshake

```sh
curl --http3 -k -v https://localhost:4433/
```

`Using HTTP/3` confirms a successful QUIC handshake.

### Force HTTP/1.1 (TCP)

```sh
curl --http1.1 -k -sI https://localhost:4433/
```

### Force HTTP/2 (TCP)

```sh
curl --http2 -k -sI https://localhost:4433/
```

### HTTP/3 Only (Fail if No QUIC)

```sh
curl --http3-only -k -sI https://localhost:4433/
```

Unlike `--http3`, this does not fall back to HTTP/2 or HTTP/1.1. If the QUIC handshake fails, curl exits with an error.

### Download a File

```sh
curl --http3 -k -o output.bin https://localhost:4433/pi
```

### POST Data

```sh
curl --http3 -k -X POST -d "key=value" https://localhost:4433/api
```

### PUT a File

```sh
curl --http3 -k -X PUT -T localfile.bin https://localhost:4433/upload/localfile.bin
```

## Alt-Svc Header

mod_http3 injects `Alt-Svc` automatically when `H3AltSvc` is enabled, which is the default. Use `mod_headers` only to override that advertisement:

```apache
Header always set Alt-Svc "h3=\":4433\"; ma=60; persist=1"
```

Verify over HTTP/2 (TCP side):

```sh
curl --http2 -k -sI https://localhost:4433/
```

## Built-in QUIC Client

The repo includes a native QUIC client (`build/bin/quic_client_test`) that connects to the standalone server (`build/bin/server`), not Apache httpd.

```sh
bash scripts/mkcert.sh keys
./build/bin/server 4433 keys/server.crt keys/server.key &
./build/bin/quic_client_test localhost 4433 keys/server.crt
```

Expected: `Status 200`, `All streams complete`.

Multiple concurrent streams:

```sh
./build/bin/quic_client_test localhost 4433 keys/server.crt 5
```

## Common Failures

| Symptom | Likely Cause | Fix |
|---|---|---|
| `curl: option --http3: is unknown` | curl built without QUIC | Build curl with HTTP/3 support |
| Response shows `HTTP/1.1` or `HTTP/2` | UDP port blocked or `--http3` missing | Check `ss -ulnp \| grep <port>` |
| `curl: (35) SSL connect error` | Certificate not trusted | Add `--cacert <cert>` or `-k` |
| `curl: (28) timed out` | UDP port blocked | Open it: `ufw allow <port>/udp` |
| `curl: (56) QUIC handshake failed` | OpenSSL version mismatch | Both sides need >= 3.5.0 |
