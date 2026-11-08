#!/usr/bin/env bash
# Build the web app in a container (no Node needed on this machine). Output: cloud/web/dist, which the backend serves.
#   cloud/web/build.sh            install + build
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e npm_config_cache=/tmp/npm -v "$PWD:/app" -w /app node:20-alpine \
    sh -c 'npm ci --no-audit --no-fund && npm run build'
