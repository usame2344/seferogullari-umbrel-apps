#!/bin/sh
set -eu

echo "Waiting for mkpool control socket..."

while [ ! -S /run/mkpool/mkpool.sock ]; do
    sleep 2
done

exec python3 /opt/yumlu/dashboard/server.py
