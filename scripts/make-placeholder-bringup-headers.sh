#!/usr/bin/env bash
# scripts/make-placeholder-bringup-headers.sh - placeholder credential headers so the production image
# BUILDS where there are no real credentials (CI). Never overwrites a file that exists.
#
# The production main (firmware/gateway/Src/main.c) needs firmware/gateway/certs/bringup_secrets.h and
# bringup_certs.h, which are git-ignored (real Wi-Fi credentials and TLS keys). An image built with these
# placeholders compiles, links and has the right size, but cannot join a network or connect to a
# broker: the Wi-Fi name and password are fake, the endpoint is a name that does not resolve, and each
# certificate array is one zero byte. For a working image use scripts/bringup_secrets.h.example and
# scripts/regenerate-bringup-certs.sh instead.
#
# Usage: scripts/make-placeholder-bringup-headers.sh
set -euo pipefail

dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/firmware/gateway/certs"
mkdir -p "$dir"

if [[ -e "$dir/bringup_secrets.h" ]]; then
    echo "kept existing $dir/bringup_secrets.h"
else
    cat > "$dir/bringup_secrets.h" <<'HDR'
/* PLACEHOLDER written by scripts/make-placeholder-bringup-headers.sh: builds, cannot connect. */
#ifndef BRINGUP_SECRETS_H
#define BRINGUP_SECRETS_H

#define BRINGUP_WIFI_SSID "placeholder-ssid"
#define BRINGUP_WIFI_PASSWORD "placeholder-password"
#define BRINGUP_REMOTE_HOST "192.0.2.1"

#endif /* BRINGUP_SECRETS_H */
HDR
    echo "wrote placeholder $dir/bringup_secrets.h"
fi

if [[ -e "$dir/bringup_certs.h" ]]; then
    echo "kept existing $dir/bringup_certs.h"
else
    cat > "$dir/bringup_certs.h" <<'HDR'
/* PLACEHOLDER written by scripts/make-placeholder-bringup-headers.sh: builds, cannot connect. */
#ifndef BRINGUP_CERTS_H
#define BRINGUP_CERTS_H

#include <stdint.h>

#define BRINGUP_CERTS_BROKER_ENDPOINT "placeholder.invalid"

static const uint8_t s_bringup_ca_cert_der[] = {0x00};
static const uint8_t s_bringup_client_cert_der[] = {0x00};
static const uint8_t s_bringup_client_key_der[] = {0x00};

#endif /* BRINGUP_CERTS_H */
HDR
    echo "wrote placeholder $dir/bringup_certs.h"
fi
