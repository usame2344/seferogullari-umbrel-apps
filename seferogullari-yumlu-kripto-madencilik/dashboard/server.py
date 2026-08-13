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
PORT = int(os.environ.get("DASHBOARD_PORT", "8095"))

DB_HOST = os.environ.get("DB_HOST", "127.0.0.1")
DB_PORT = int(os.environ.get("DB_PORT", "5433"))
DB_NAME = os.environ.get("DB_NAME", "mkpool")
DB_USER = os.environ.get("DB_USER", "mkpool_user")
DB_PASS = os.environ.get("DB_PASS", "")

MKPOOL_CTL = os.environ.get(
    "MKPOOL_CTL",
    "/opt/yumlu/mkpool-src/scripts/mkpool-ctl.py"
)
MKPOOL_SOCKET = os.environ.get(
    "MKPOOL_SOCKET",
    "/run/mkpool/mkpool.sock"
)

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
    db_hashrate_1m = 0.0
    db_hashrate_5m = 0.0
    db_workers = 0

    try:
        conn = db()
        cur = conn.cursor()

        cur.execute("""
            SELECT COUNT(*)
            FROM blocks
            WHERE height > 0
        """)
        blocks = int(cur.fetchone()[0] or 0)

        cur.execute("""
            SELECT COALESCE(MAX(best_share_difficulty), 0)
            FROM miners
        """)
        db_best = float(cur.fetchone()[0] or 0)

        # Share difficulty -> estimated hashrate.
        # H/s = sum(difficulty) * 2^32 / elapsed seconds.
        cur.execute("""
            SELECT COALESCE(
                SUM(difficulty) FILTER (
                    WHERE accepted
                    AND created_at >= NOW() - INTERVAL '1 minute'
                ), 0
            )
            FROM raw_shares
        """)
        diff_1m = float(cur.fetchone()[0] or 0)
        db_hashrate_1m = diff_1m * 4294967296.0 / 60.0

        cur.execute("""
            SELECT COALESCE(
                SUM(difficulty) FILTER (
                    WHERE accepted
                    AND created_at >= NOW() - INTERVAL '5 minutes'
                ), 0
            )
            FROM raw_shares
        """)
        diff_5m = float(cur.fetchone()[0] or 0)
        db_hashrate_5m = diff_5m * 4294967296.0 / 300.0

        # Son 2 dakika içinde accepted share gönderen worker = aktif worker.
        cur.execute("""
            SELECT COUNT(DISTINCT miner_id)
            FROM raw_shares
            WHERE accepted
              AND created_at >= NOW() - INTERVAL '2 minutes'
        """)
        db_workers = int(cur.fetchone()[0] or 0)

        cur.close()
        conn.close()

    except Exception:
        pass

    ctl_hashrate_1m = float(stats.get("hashrate_1m") or 0)
    ctl_hashrate_5m = float(stats.get("hashrate_5m") or 0)
    ctl_workers = int(stats.get("authorized") or 0)

    return {
        # Control socket öncelikli; yoksa DB fallback.
        "hashrate_1m": (
            ctl_hashrate_1m
            if ctl_hashrate_1m > 0
            else db_hashrate_1m
        ),
        "hashrate_5m": (
            ctl_hashrate_5m
            if ctl_hashrate_5m > 0
            else db_hashrate_5m
        ),
        "workers": (
            ctl_workers
            if ctl_workers > 0
            else db_workers
        ),
        "connections": int(stats.get("connections") or db_workers),
        "best_share": max(best_round, db_best),
        "blocks": blocks,
        "uptime_seconds": int(stats.get("uptime_seconds") or 0),

        # DB çalışıyorsa pool'u yine aktif kabul ediyoruz.
        "online": bool(stats) or db_workers > 0
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
                COALESCE((
                    SELECT MAX(rs.created_at)
                    FROM raw_shares rs
                    WHERE rs.miner_id = m.id
                      AND rs.accepted = TRUE
                ), m.last_share_at) AS last_share_at,
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



def analytics():
    data = {
        "accepted_1h": 0,
        "rejected_1h": 0,
        "accepted_24h": 0,
        "rejected_24h": 0,
        "round_diff": 0.0,
        "round_effort_pct": 0.0,
        "network_difficulty": 0.0,
        "network_hashrate": 0.0,
        "block_height": 0,
        "block_reward": 0.0,
        "network_updated_at": None,
        "avg_hashrate_6h": 0.0,
        "peak_hashrate_6h": 0.0,
        "last_share_at": None
    }

    try:
        conn = db()
        cur = conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor)

        cur.execute("""
            SELECT
                COUNT(*) FILTER (
                    WHERE accepted
                    AND created_at >= NOW() - INTERVAL '1 hour'
                ) AS accepted_1h,

                COUNT(*) FILTER (
                    WHERE NOT accepted
                    AND created_at >= NOW() - INTERVAL '1 hour'
                ) AS rejected_1h,

                COUNT(*) FILTER (
                    WHERE accepted
                    AND created_at >= NOW() - INTERVAL '24 hours'
                ) AS accepted_24h,

                COUNT(*) FILTER (
                    WHERE NOT accepted
                    AND created_at >= NOW() - INTERVAL '24 hours'
                ) AS rejected_24h,

                MAX(created_at) FILTER (WHERE accepted) AS last_share_at
            FROM raw_shares
        """)

        row = cur.fetchone()

        if row:
            for k in (
                "accepted_1h",
                "rejected_1h",
                "accepted_24h",
                "rejected_24h"
            ):
                data[k] = int(row[k] or 0)

            if row["last_share_at"]:
                data["last_share_at"] = row["last_share_at"].isoformat()

        cur.execute("""
            SELECT COALESCE(accum_diff, 0) AS accum_diff
            FROM effort_state
            ORDER BY id
            LIMIT 1
        """)

        row = cur.fetchone()
        if row:
            data["round_diff"] = float(row["accum_diff"] or 0)

        cur.execute("""
            SELECT
                network_difficulty,
                network_hashrate,
                block_height,
                block_reward,
                updated_at
            FROM network_stats
            WHERE UPPER(coin) = 'BTC'
            LIMIT 1
        """)

        row = cur.fetchone()

        if row:
            data["network_difficulty"] = float(
                row["network_difficulty"] or 0
            )
            data["network_hashrate"] = float(
                row["network_hashrate"] or 0
            )
            data["block_height"] = int(
                row["block_height"] or 0
            )
            data["block_reward"] = float(
                row["block_reward"] or 0
            )

            if row["updated_at"]:
                data["network_updated_at"] = (
                    row["updated_at"].isoformat()
                )

        if data["network_difficulty"] > 0:
            data["round_effort_pct"] = (
                data["round_diff"]
                / data["network_difficulty"]
                * 100.0
            )

        cur.execute("""
            SELECT
                COALESCE(AVG(pool_hr), 0) AS avg_hr,
                COALESCE(MAX(pool_hr), 0) AS peak_hr
            FROM (
                SELECT
                    created_at,
                    SUM(hashrate) AS pool_hr
                FROM hashrates
                WHERE created_at >= NOW() - INTERVAL '6 hours'
                GROUP BY created_at
            ) q
        """)

        row = cur.fetchone()

        if row:
            data["avg_hashrate_6h"] = float(row["avg_hr"] or 0)
            data["peak_hashrate_6h"] = float(row["peak_hr"] or 0)

        cur.close()
        conn.close()

    except Exception as e:
        data["error"] = str(e)

    return data

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
                b.net_difficulty,
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

        if parsed.path == "/api/analytics":
            return self.json_response(analytics())

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
