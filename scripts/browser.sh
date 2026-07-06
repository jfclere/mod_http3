#!/bin/bash
set -eu

cd "$(dirname "$0")/.."

HTTPD_PATH="${1:-$(pwd)/dependencies/httpd-dist}"
HTTPD_PATH="$(cd "$HTTPD_PATH" && pwd)"
HTTPD_PORT="8443"
URL="https://localhost:$HTTPD_PORT/"
CRT="$HTTPD_PATH/conf/certs/server.crt"
BROWSER="chromium-browser" # or "google-chrome"
PROFILE="$HOME/.cache/mod-http3-chrome-dev"
rm -rf "$PROFILE"

if [ ! -f "$CRT" ]; then
    echo "error: no certs found. run ./scripts/mkcert.sh $CRT first" >&2
    exit 1
fi

SPKI=$(openssl x509 -in "$CRT" -pubkey -noout \
    | openssl pkey -pubin -outform der \
    | openssl dgst -sha256 -binary | base64)

echo "Launching browser for local mod_http3 development..."

exec "$BROWSER" \
    --user-data-dir="$PROFILE" \
    --ignore-certificate-errors \
    --allow-insecure-localhost \
    --ignore-certificate-errors-spki-list="$SPKI" \
    --origin-to-force-quic-on=localhost:$HTTPD_PORT \
    "$URL"
