#!/usr/bin/env python3
import json, os, socket, time
from datetime import datetime, timezone
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
import psycopg2
import psycopg2.extras

BASE = os.path.dirname(__file__)
SOCKET_PATH = os.getenv("MKPOOL_CONTROL_SOCKET", "/run/mkpool/mkpool.sock")
DB = dict(
    host=os.getenv("DB_HOST", "db"), port=int(os.getenv("DB_PORT", "5432")),
    dbname=os.getenv("DB_NAME", "mkpool"), user=os.getenv("DB_USER", "mkpool_user"),
    password=os.getenv("DB_PASS", ""), connect_timeout=3
)

def ctl(command):
    try:
        s=socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(2); s.connect(SOCKET_PATH)
        s.sendall((command+"\n").encode()); chunks=[]
        while True:
            b=s.recv(65536)
            if not b: break
            chunks.append(b)
        s.close(); raw=b"".join(chunks).decode(errors="replace").strip()
        return json.loads(raw) if raw else {}
    except Exception:
        return {}

def db_query(sql, params=()):
    with psycopg2.connect(**DB) as conn:
        with conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute(sql, params); return [dict(r) for r in cur.fetchall()]

def jsonable(v):
    if isinstance(v, datetime): return v.astimezone(timezone.utc).isoformat()
    return v

def sanitize(rows):
    return [{k:jsonable(v) for k,v in r.items()} for r in rows]

def summary():
    st=ctl("stats")
    try:
        row=db_query("""
          SELECT
            (SELECT count(*) FROM blocks) AS block_count,
            COALESCE((SELECT max(best_share_difficulty) FROM miners),0) AS best_all_time,
            COALESCE((SELECT count(*) FROM miners WHERE status='online'),0) AS db_online
        """)[0]
    except Exception:
        row={"block_count":0,"best_all_time":0,"db_online":0}
    coin=(st.get("coins") or [{}])[0]
    return {
      "hashrate_1m": st.get("hashrate_1m",0), "hashrate_5m": st.get("hashrate_5m",0),
      "active_workers": st.get("authorized", row.get("db_online",0)),
      "connections": st.get("connections",0), "uptime_seconds": st.get("uptime_seconds",0),
      "best_share_round": coin.get("best_share_round",0), "best_share_all_time": row.get("best_all_time",0),
      "block_count": row.get("block_count",0), "stratum_port": 3333
    }

def workers():
    sql="""
      SELECT m.id,m.worker_name,m.btc_address,m.status,m.best_share_difficulty,m.best_share_hash,
             m.last_share_at,m.updated_at,
             COALESCE(h.hashrate,0) AS hashrate,
             COALESCE(s.accepted,0) AS accepted,
             COALESCE(s.rejected,0) AS rejected
      FROM miners m
      LEFT JOIN LATERAL (
        SELECT hashrate FROM hashrates h WHERE h.miner_id=m.id ORDER BY created_at DESC LIMIT 1
      ) h ON true
      LEFT JOIN LATERAL (
        SELECT count(*) FILTER (WHERE accepted) accepted,
               count(*) FILTER (WHERE NOT accepted) rejected
        FROM raw_shares r WHERE r.miner_id=m.id AND r.created_at > now()-interval '6 hours'
      ) s ON true
      ORDER BY (m.status='online') DESC, m.updated_at DESC
    """
    return sanitize(db_query(sql))

def graph(range_name):
    settings={"1h":("1 minute","1 hour"),"6h":("5 minutes","6 hours"),"24h":("15 minutes","24 hours")}
    bucket, span=settings.get(range_name,settings["6h"])
    sql="""
      SELECT time_bucket(%s::interval, created_at) AS t, sum(hashrate) AS hashrate
      FROM hashrates WHERE created_at > now()-(%s::interval)
      GROUP BY t ORDER BY t
    """
    return sanitize(db_query(sql,(bucket,span)))

def blocks():
    sql="""
      SELECT b.height,b.block_hash,b.reward_value,b.found_at,b.round_effort,b.finder_effort,
             b.difficulty,b.net_difficulty,b.coin,m.worker_name,m.btc_address
      FROM blocks b LEFT JOIN miners m ON m.id=b.found_by
      ORDER BY b.found_at DESC LIMIT 20
    """
    return sanitize(db_query(sql))

class Handler(SimpleHTTPRequestHandler):
    def translate_path(self,path):
        rel=os.path.normpath(urlparse(path).path.lstrip('/') or 'index.html')
        if rel.startswith('..'): rel='index.html'
        return os.path.join(BASE,'static',rel)
    def send_json(self,obj,status=200):
        body=json.dumps(obj,ensure_ascii=False).encode(); self.send_response(status)
        self.send_header('Content-Type','application/json; charset=utf-8'); self.send_header('Cache-Control','no-store')
        self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        p=urlparse(self.path)
        try:
            if p.path=='/api/summary': return self.send_json(summary())
            if p.path=='/api/workers': return self.send_json(workers())
            if p.path=='/api/hashrate': return self.send_json(graph(parse_qs(p.query).get('range',['6h'])[0]))
            if p.path=='/api/blocks': return self.send_json(blocks())
            if p.path=='/api/health':
                st=ctl('ping'); return self.send_json({'ok':bool(st or os.path.exists(SOCKET_PATH)),'time':time.time()})
            return super().do_GET()
        except Exception as e:
            return self.send_json({'error':str(e)},500)
    def log_message(self,fmt,*args): pass

if __name__=='__main__':
    port=int(os.getenv('DASHBOARD_PORT','8080'))
    print(f"YUMLU dashboard listening on :{port}",flush=True)
    ThreadingHTTPServer(('0.0.0.0',port),Handler).serve_forever()
