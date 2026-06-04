# mod_http3 httpd Configuration

Apache httpd configuration directives for mod_http3.

For build and installation, see [INSTALL](INSTALL).

## Overview

mod_http3 enables HTTP/3 protocol support in Apache HTTP Server. The module:

- Creates a separate worker thread for HTTP/3 connections over UDP/QUIC
- Uses OpenSSL for QUIC/TLS 1.3 support
- Uses nghttp3 for HTTP/3 protocol handling
- Integrates with Apache's standard request processing pipeline

## Configuration Directives

### H3CertificatePath

**Syntax:** `H3CertificatePath /path/to/certificate.pem`
**Context:** server config, virtual host
**Required:** Yes

Path to the TLS certificate file for HTTP/3 connections. May point to the same file used by `SSLCertificateFile`.

### H3CertificateKeyPath

**Syntax:** `H3CertificateKeyPath /path/to/private-key.pem`
**Context:** server config, virtual host
**Required:** Yes

Path to the TLS private key file for HTTP/3 connections. May point to the same file used by `SSLCertificateKeyFile`.

## VirtualHost Configuration

### Port Detection

The module automatically detects the port from the VirtualHost configuration:

```apache
# HTTP/3 will listen on port 8443
<VirtualHost *:8443>
    ServerName secure.example.com
    H3CertificatePath /etc/httpd/ssl/secure.crt
    H3CertificateKeyPath /etc/httpd/ssl/secure.key
</VirtualHost>
```

If no port is specified, the module defaults to **port 4433**.

### Multiple VirtualHosts

The module uses the **first VirtualHost** that has both `H3CertificatePath` and `H3CertificateKeyPath` configured:

```apache
# This VirtualHost is used for HTTP/3
<VirtualHost *:4433>
    ServerName primary.example.com
    H3CertificatePath /etc/httpd/ssl/primary.crt
    H3CertificateKeyPath /etc/httpd/ssl/primary.key
</VirtualHost>

# This VirtualHost is ignored for HTTP/3
<VirtualHost *:4433>
    ServerName secondary.example.com
    H3CertificatePath /etc/httpd/ssl/secondary.crt
    H3CertificateKeyPath /etc/httpd/ssl/secondary.key
</VirtualHost>
```

### Alt-Svc Header

mod_http3 does not inject the `Alt-Svc` header automatically. Add it via `mod_headers`:

```apache
Header always set Alt-Svc "h3=\":4433\"; ma=60; persist=1"
```

| Field | Meaning |
|---|---|
| `h3=":4433"` | HTTP/3 available on same host, port 4433 |
| `ma=60` | Advertise for 60 seconds |
| `persist=1` | Persist across network changes |

### EnableMMAP

`EnableMMAP Off` is **required**. The QUIC response path reads file content via `apr_file_read`. With MMAP active, the file bucket bypasses the QUIC engine and corrupts the response.

## Troubleshooting

### Startup Validation

The module validates configuration during Apache startup:

1. **Certificate Path Check:** `H3CertificatePath` is configured
2. **Key Path Check:** `H3CertificateKeyPath` is configured

If either check fails, Apache refuses to start.

### Testing Configuration

```sh
httpd -t                              # test syntax
httpd -t -D DUMP_VHOSTS               # verbose
httpd -M | grep http3                 # check module
```

### Verifying HTTP/3 Operation

```sh
ss -ulnp | grep httpd                 # check UDP listener
curl --http3 -k https://localhost:4433/   # test transfer
tail -f /var/log/httpd/error_log      # watch logs
```

### Debug Logging

```apache
LogLevel http3:trace8
```

### Log Messages

```
# Successful configuration
h3_post_config: [PID] cert_path=/path/to/cert key_path=/path/to/key

# Worker thread started
h3_child_init
worker_thread_main

# Errors
mod_http3: H3CertificatePath directive is required but not configured
mod_http3: H3CertificateKeyPath directive is required but not configured
```

### Security

```sh
# Set restrictive permissions
chmod 600 /etc/httpd/ssl/server.key
chown root:root /etc/httpd/ssl/server.key

# Or if Apache runs as a different user
chown apache:apache /etc/httpd/ssl/server.key
```
