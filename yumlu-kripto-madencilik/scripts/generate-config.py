#!/usr/bin/env python3
import json, os, pathlib

def env(name, default=''):
    return os.environ.get(name, default)

cfg = {
  "activeCoin": "BTC",
  "global": {
    "role": "pool",
    "metricsListenAddress": "0.0.0.0",
    "metricsListenPort": 9090,
    "logPath": "/data/logs",
    "logLevel": 2,
    "databaseHost": env("DB_HOST", "db"),
    "databasePort": int(env("DB_PORT", "5432")),
    "databaseName": env("DB_NAME", "mkpool"),
    "databaseUser": env("DB_USER", "mkpool_user"),
    "databasePassword": env("DB_PASS"),
    "ioThreads": 2,
    "sessionShards": 16,
    "controlSocket": "/run/mkpool/mkpool.sock",
    "idleDropSeconds": 0
  },
  "coins": {
    "BTC": {
      "chain": "bitcoin",
      "rpcHost": env("BITCOIN_RPC_HOST"),
      "rpcPort": env("BITCOIN_RPC_PORT", "8332"),
      "rpcUser": env("BITCOIN_RPC_USER"),
      "rpcPassword": env("BITCOIN_RPC_PASS"),
      "useZMQ": True,
      "zmq": {
        "hashblock": [f"tcp://{env('BITCOIN_ZMQ_HOST')}:{env('BITCOIN_ZMQ_HASHBLOCK_PORT','28334')}"],
        "rawblock": [f"tcp://{env('BITCOIN_ZMQ_HOST')}:{env('BITCOIN_ZMQ_RAWBLOCK_PORT','28332')}"]
      },
      "stratumListenAddress": "0.0.0.0",
      "stratumListenPort": 3333,
      "stratumV2Port": 0,
      "stratumTiers": [{
        "port": 3333,
        "label": "vardiff",
        "startingDifficulty": 1024,
        "vardiffEnabled": True,
        "vardiffMin": 1024,
        "vardiffMax": 10000000
      }],
      "targetSharesPerMinute": 12.0,
      "vardiffTauSeconds": 30.0,
      "blockPollInterval": 10,
      "coinbaseSignature": "/SEFEROGULLARI/",
      "donationPercent": 0.0,
      "donationAddress": "",
      "enableVersionRolling": True,
      "versionRollingMask": "1fffe000",
      "jobWindowSize": 32,
      "additionalSubmitEndpoints": []
    }
  }
}

required = ["BITCOIN_RPC_HOST","BITCOIN_RPC_USER","BITCOIN_RPC_PASS","BITCOIN_ZMQ_HOST","DB_PASS"]
missing = [k for k in required if not env(k)]
if missing:
    raise SystemExit("Missing required environment variables: " + ", ".join(missing))

path = pathlib.Path(env("MKPOOL_CONFIG", "/data/config.json"))
path.parent.mkdir(parents=True, exist_ok=True)
path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
print(f"Generated {path}")
