#!/bin/sh
set -eu

mkdir -p /data/logs /run/mkpool

echo "Waiting for TimescaleDB..."

until python3 - <<'PY'
import os
import psycopg2

psycopg2.connect(
    host=os.environ["DB_HOST"],
    port=int(os.environ.get("DB_PORT", "5432")),
    dbname=os.environ["DB_NAME"],
    user=os.environ["DB_USER"],
    password=os.environ["DB_PASS"],
    connect_timeout=2
).close()
PY
do
    sleep 2
done

python3 /opt/yumlu/scripts/generate-config.py

echo "Starting YUMLU KRIPTO MADENCILIK..."
exec /usr/local/bin/mkpool --config /data/config.json
