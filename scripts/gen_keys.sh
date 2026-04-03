#!/bin/sh
# Generate a self-signed TLS certificate and private key for local testing.
#
# Output (written to keys/ relative to the current working directory):
#   keys/privkey.pem  -- RSA-2048 private key
#   keys/pubcert.pem  -- self-signed X.509 certificate (CN=localhost, 365 days)
#
# These paths match the defaults expected by httpd.py and the run_* scripts.
# Requires: openssl on PATH (any reasonably modern version).
set -e

KEYS_DIR="${1:-keys}"

if ! command -v openssl > /dev/null 2>&1; then
    echo "error: openssl not found on PATH" >&2
    exit 1
fi

mkdir -p "$KEYS_DIR"

KEYFILE="$KEYS_DIR/privkey.pem"
CERTFILE="$KEYS_DIR/pubcert.pem"

# Refuse to overwrite existing keys unless -f is passed.
if [ -f "$KEYFILE" ] || [ -f "$CERTFILE" ]; then
    if [ "${FORCE:-0}" != "1" ]; then
        echo "error: keys already exist in '$KEYS_DIR/' — set FORCE=1 to overwrite" >&2
        exit 1
    fi
    echo "FORCE=1 set — overwriting existing keys in '$KEYS_DIR/'"
fi

openssl req \
    -x509 \
    -newkey rsa:2048 \
    -keyout "$KEYFILE" \
    -out    "$CERTFILE" \
    -days   365 \
    -nodes \
    -subj   "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1"

echo ""
echo "Generated:"
echo "  private key : $KEYFILE"
echo "  certificate : $CERTFILE"
echo ""
echo "These are self-signed and intended for local testing only."
echo "To trust them for the QUIC client, install the certificate"
echo "into your system CA store or pass it to SSL_CTX_load_verify_locations."
