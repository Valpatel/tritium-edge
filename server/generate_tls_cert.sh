#!/bin/bash
# Generate self-signed TLS certificate for the fleet server.
# Usage: ./generate_tls_cert.sh [output_dir]
# Creates: server.crt and server.key

set -e
DIR="${1:-./fleet_data}"
mkdir -p "$DIR"

if [ -f "$DIR/server.crt" ] && [ -f "$DIR/server.key" ]; then
    echo "TLS cert already exists in $DIR/"
    echo "  Certificate: $DIR/server.crt"
    echo "  Private key: $DIR/server.key"
    echo "Delete them first to regenerate."
    exit 0
fi

echo "Generating self-signed TLS certificate..."
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "$DIR/server.key" -out "$DIR/server.crt" \
    -days 365 -nodes \
    -subj "/CN=ESP32 Fleet Server/O=Local/C=US" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

chmod 600 "$DIR/server.key"
echo "Done!"
echo "  Certificate: $DIR/server.crt"
echo "  Private key: $DIR/server.key"
echo ""
echo "Start server with TLS:"
echo "  python3 fleet_server.py --ssl-cert $DIR/server.crt --ssl-key $DIR/server.key"
