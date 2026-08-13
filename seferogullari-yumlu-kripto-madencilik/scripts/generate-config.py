#!/usr/bin/env python3

import json
import os

cfg = {
    "activeCoin": "BTC",

    "global": {
        "role": "pool",

        "metricsListenAddress": "0.0.0.0",
        "metricsListenPort": 9090,

        "logPath": "/data/logs",
        "logLevel": 2,

        "databaseHost": os.environ["DB_HOST"],
        "databasePort": int(os.environ.get("DB_PORT", "5432")),
        "databaseName": os.environ["DB_NAME"],
        "databaseUser": os.environ["DB_USER"],
        "databasePassword": os.environ["DB_PASS"],

        "ioThreads": 2,
        "sessionShards": 16,

        "controlSocket": "/run/mkpool/mkpool.sock",
        "idleDropSeconds": 0
    },

    "coins": {
        "BTC": {
            "chain": "bitcoin",

            "rpcHost": os.environ["BITCOIN_RPC_HOST"],
            "rpcPort": os.environ.get("BITCOIN_RPC_PORT", "8332"),
            "rpcUser": os.environ["BITCOIN_RPC_USER"],
            "rpcPassword": os.environ["BITCOIN_RPC_PASS"],

            "useZMQ": True,

            "zmq": {
                "hashblock": [
                    "tcp://{}:{}".format(
                        os.environ["BITCOIN_ZMQ_HOST"],
                        os.environ["BITCOIN_ZMQ_HASHBLOCK_PORT"]
                    )
                ],

                "rawblock": [
                    "tcp://{}:{}".format(
                        os.environ["BITCOIN_ZMQ_HOST"],
                        os.environ["BITCOIN_ZMQ_RAWBLOCK_PORT"]
                    )
                ]
            },

            "stratumListenAddress": "0.0.0.0",
            "stratumListenPort": 3333,

            "stratumV2Port": 0,

            "stratumTiers": [
                {
                    "port": 3333,
                    "label": "vardiff",
                    "startingDifficulty": 1024,
                    "vardiffEnabled": True,
                    "vardiffMin": 1024,
                    "vardiffMax": 10000000
                }
            ],

            "targetSharesPerMinute": 12.0,
            "vardiffTauSeconds": 30.0,
            "blockPollInterval": 10,

            "coinbaseSignature": "/SEFEROGULLARI/",
            "donationPercent": 0.0,
            "donationAddress": "bc1qn7kmd374qmlu5w38nat8a8an9rx679gsz7qa8m",

            "enableVersionRolling": True,
            "versionRollingMask": "1fffe000",
            "jobWindowSize": 32,

            "additionalSubmitEndpoints": []
        }
    }
}

os.makedirs("/data", exist_ok=True)

with open("/data/config.json", "w") as f:
    json.dump(cfg, f, indent=2)

print("YUMLU mkpool config generated.")
