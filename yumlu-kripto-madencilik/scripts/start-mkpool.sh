#!/usr/bin/env bash
set -euo pipefail
mkdir -p /data/logs /run/mkpool
python3 /opt/yumlu/generate-config.py
exec /usr/local/bin/mkpool --config /data/config.json
