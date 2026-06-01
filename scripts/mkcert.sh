#!/bin/sh
set -eu

OUT_DIR="${1:-certs}"
KEYFILE="${OUT_DIR}/server.key"
CRTFILE="${OUT_DIR}/server.crt"

command -v openssl > /dev/null 2>&1 || { echo "openssl not found on PATH" >&2; exit 1; }

mkdir -p "${OUT_DIR}"

if [ -f "$KEYFILE" ] || [ -f "$CRTFILE" ]; then
    echo "error: keys already exist in '$OUT_DIR/'" >&2
    exit 1
fi

openssl req \
    -x509 \
    -newkey rsa:4096 \
    -days 365 \
    -nodes \
    -keyout "${KEYFILE}" \
    -out    "${CRTFILE}" \
    -subj   "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1"

chmod 600 "${KEYFILE}"

echo "Generated:"
echo "  private key : $KEYFILE"
echo "  public  crt : $CRTFILE"
