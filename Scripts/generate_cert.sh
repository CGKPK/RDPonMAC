#!/bin/bash
# Generate a self-signed TLS certificate for RDPonMAC

CERT_DIR="$HOME/Library/Application Support/RDPonMAC/certs"
mkdir -p "$CERT_DIR"

CERT_FILE="$CERT_DIR/server.crt"
KEY_FILE="$CERT_DIR/server.key"

if [ -f "$CERT_FILE" ] && [ -f "$KEY_FILE" ]; then
    echo "Certificate already exists at $CERT_DIR"
    echo "To regenerate, delete the existing files first."
    exit 0
fi

openssl req -x509 -newkey rsa:2048 \
    -keyout "$KEY_FILE" \
    -out "$CERT_FILE" \
    -days 365 \
    -nodes \
    -subj "/CN=RDPonMAC"

if [ $? -eq 0 ]; then
    echo "Certificate generated successfully:"
    echo "  Certificate: $CERT_FILE"
    echo "  Private Key: $KEY_FILE"
else
    echo "Failed to generate certificate"
    exit 1
fi
