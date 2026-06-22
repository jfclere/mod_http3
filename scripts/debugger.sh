#!/bin/bash
set -eu

cd "$(dirname "$0")/.."

HTTPD_PATH="${1:-$(pwd)/dependencies/httpd-dist}"

if [ ! -d "$HTTPD_PATH" ]; then
    echo "Error: httpd path does not exist: $HTTPD_PATH" >&2
    echo "Usage: $0 [path-to-httpd]" >&2
    exit 1
fi
HTTPD_PATH="$(cd "$HTTPD_PATH" && pwd)"

echo "=== Using httpd at: $HTTPD_PATH ==="

echo "=== Building mod_http3 ==="
cmake --build build

echo "=== Deploying module ==="
cp build/lib/mod_http3.so "$HTTPD_PATH/modules/mod_http3.so"

echo "=== Deploying conf ==="
mkdir -p "$HTTPD_PATH/conf"
# Replace the container's /src/dependencies/httpd-dist prefix with the real path.
sed "s|/src/dependencies/httpd-dist|$HTTPD_PATH|g" container/httpd.conf > "$HTTPD_PATH/conf/httpd.conf"

echo "=== Checking certificates ==="
if [ ! -f "$HTTPD_PATH/conf/certs/server.crt" ] || [ ! -f "$HTTPD_PATH/conf/certs/server.key" ]; then
    scripts/mkcert.sh "$HTTPD_PATH/conf/certs"
fi

echo "=== Ready to debug ==="
echo "Launch GDB with:"
echo "  gdb --args $HTTPD_PATH/bin/httpd -X -f $HTTPD_PATH/conf/httpd.conf"
echo ""
echo "Or run directly:"
echo "  $HTTPD_PATH/bin/httpd -X -f $HTTPD_PATH/conf/httpd.conf"
echo ""
