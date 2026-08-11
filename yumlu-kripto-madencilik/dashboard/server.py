#!/usr/bin/env python3

import json
import os
import subprocess
from datetime import datetime
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

import psycopg2
import psycopg2.extras

HOST = "0.0.0.0"
PORT = 8095

DB_HOST = os.environ.get("DB_HOST", "127.0.0.1")
DB_PORT = int(os.environ.get("DB_PORT", "5433"))
DB_NAME = os.environ.get("DB_NAME", "mkpool")
DB_USER = os.environ.get("DB_USER", "mkpool_user")
DB_PASS = os.environ.get("DB_PASS", "")

MKPOOL_CTL = os.path.expanduser(
    "~/mkpool-build/mkpool/scripts/mkpool-ctl.py"
)
MKPOOL_SOCKET = "/tmp/yumlubtc-mkpool.sock"

STATIC_DIR = os.path.join(os.path.dirname(__file__), "static")


def db():
    return psycopg2.connect(
        host=DB_HOST,
        port=DB_PORT,
        dbname=DB_NAME,
        user=DB_USER,
        password=DB_PASS,
        connect_timeout=3
    )


def ctl(command):
    try:
        out = subprocess.check_output(
            [
                "python3",
                MKPOOL_CTL,
                "--socket",
                MKPOOL_SOCKET,
                command
            ],
            text=True,
            timeout=3,
            stderr=subprocess.DEVNULL
        )
        return json.loads(out)
    except Exception:
        return {}


def overview():
    stats = ctl("stats")

    best_round = 0.0
    for coin in stats.get("coins", []):
        if coin.get("name") == "BTC":
            best_round = float(coin.get("best_share_round") or 0)

    blocks = 0
    db_best = 0.0

    try:
        conn = db()
        cur = conn.cursor()

        cur.execute("SELECT COUNT(*) FROM blocks WHERE height > 0")
        blocks = cur.fetchone()[0]

        cur.execute("""
            SELECT COALESCE(MAX(best_share_difficulty), 0)
            FROM miners
        """)
        db_best = float(cur.fetchone()[0] or 0)

        cur.close()
        conn.close()
    except Exception:
        pass

    return {
        "hashrate_1m": float(stats.get("hashrate_1m") or 0),
        "hashrate_5m": float(stats.get("hashrate_5m") or 0),
        "workers": int(stats.get("authorized") or 0),
        "connections": int(stats.get("connections") or 0),
        "best_share": max(best_round, db_best),
        "blocks": blocks,
        "uptime_seconds": int(stats.get("uptime_seconds") or 0),
        "online": bool(stats)
    }


def miners():
    try:
        conn = db()
        cur = conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor)

        cur.execute("""
            SELECT
                m.id,
                m.btc_address,
                COALESCE(NULLIF(m.worker_name, ''), 'worker') AS worker_name,
                m.status::text AS status,
                COALESCE(m.best_share_difficulty, 0) AS best_share_difficulty,
                m.last_share_at,
                COALESCE((
                    SELECT h.hashrate
                    FROM hashrates h
                    WHERE h.miner_id = m.id
                    ORDER BY h.created_at DESC
                    LIMIT 1
                ), 0) AS hashrate
            FROM miners m
            ORDER BY m.updated_at DESC
        """)

        rows = cur.fetchall()

        cur.close()
        conn.close()

        for r in rows:
            if r["last_share_at"]:
                r["last_share_at"] = r["last_share_at"].isoformat()

        return rows
    except Exception:
        return []


def blocks():
    try:
        conn = db()
        cur = conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor)

        cur.execute("""
            SELECT
                b.height,
                b.block_hash,
                COALESCE(m.worker_name, '') AS worker_name,
                COALESCE(m.btc_address, '') AS btc_address,
                b.reward_value,
                b.difficulty,
                b.round_effort,
                b.finder_effort,
                b.found_at
            FROM blocks b
            LEFT JOIN miners m ON m.id = b.found_by
            WHERE b.height > 0
            ORDER BY b.height DESC
            LIMIT 25
        """)

        rows = cur.fetchall()

        cur.close()
        conn.close()

        for r in rows:
            if r["found_at"]:
                r["found_at"] = r["found_at"].isoformat()

        return rows
    except Exception:
        return []


def history(hours):
    hours = max(1, min(hours, 24))

    try:
        conn = db()
        cur = conn.cursor()

        cur.execute("""
            SELECT
                EXTRACT(
                    EPOCH FROM time_bucket('1 minute', created_at)
                )::BIGINT AS ts,
                COALESCE(SUM(hashrate), 0) AS hashrate
            FROM hashrates
            WHERE created_at >= NOW() - (%s * INTERVAL '1 hour')
            GROUP BY 1
            ORDER BY 1
        """, (hours,))

        result = [
            {
                "time": int(row[0]),
                "hashrate": float(row[1])
            }
            for row in cur.fetchall()
        ]

        cur.close()
        conn.close()

        return result
    except Exception:
        return []


class Handler(SimpleHTTPRequestHandler):

    def translate_path(self, path):
        parsed = urlparse(path)

        if parsed.path == "/":
            return os.path.join(STATIC_DIR, "index.html")

        return os.path.join(
            STATIC_DIR,
            parsed.path.lstrip("/")
        )

    def json_response(self, data):
        body = json.dumps(
            data,
            ensure_ascii=False,
            default=str
        ).encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)

        if parsed.path == "/api/overview":
            return self.json_response(overview())

        if parsed.path == "/api/miners":
            return self.json_response(miners())

        if parsed.path == "/api/blocks":
            return self.json_response(blocks())

        if parsed.path == "/api/history":
            args = parse_qs(parsed.query)

            try:
                hours = int(args.get("hours", ["1"])[0])
            except Exception:
                hours = 1

            return self.json_response(history(hours))

        return super().do_GET()

    def log_message(self, fmt, *args):
        return


if __name__ == "__main__":
    print(f"YUMLU KRİPTO MADENCİLİK dashboard :{PORT}")
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()
