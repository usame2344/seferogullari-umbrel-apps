#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0
# mkpool-ctl - talk to a running mkpool instance's control socket.
#
# Usage:
#   mkpool-ctl.py [-i INSTANCE | -s SOCKET] [-r] COMMAND [ARGS...]
#
# Examples:
#   mkpool-ctl.py -i btc-testnet ping
#   mkpool-ctl.py -i btc-mainnet stats
#   mkpool-ctl.py -i btc-testnet clients
#   mkpool-ctl.py -i btc-testnet reconnect              # client.reconnect to all
#   mkpool-ctl.py -i btc-testnet dropclient 5
#   mkpool-ctl.py -s /run/mkpool/btc-testnet.sock loglevel debug
#
# The default socket path is /run/mkpool/<INSTANCE>.sock.
import argparse
import json
import socket
import sys


def main():
    ap = argparse.ArgumentParser(description="mkpool control-socket client")
    ap.add_argument("-i", "--instance", help="instance name -> /run/mkpool/<name>.sock")
    ap.add_argument("-s", "--socket", help="explicit unix socket path")
    ap.add_argument("-r", "--raw", action="store_true", help="print raw response (no pretty JSON)")
    ap.add_argument("command", nargs=argparse.REMAINDER, help="command and args (try: help)")
    a = ap.parse_args()

    path = a.socket or (f"/run/mkpool/{a.instance}.sock" if a.instance else None)
    if not path:
        ap.error("need -i INSTANCE or -s SOCKET")
    if not a.command:
        ap.error("need a command (try: help)")

    line = " ".join(a.command) + "\n"
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect(path)
        s.sendall(line.encode())
        chunks = []
        while True:
            b = s.recv(65536)
            if not b:
                break
            chunks.append(b)
        s.close()
    except OSError as e:
        print(f"mkpool-ctl: {path}: {e}", file=sys.stderr)
        sys.exit(1)

    data = b"".join(chunks).decode(errors="replace").strip()
    if a.raw:
        print(data)
        return
    try:
        print(json.dumps(json.loads(data), indent=2))
    except Exception:
        print(data)


if __name__ == "__main__":
    main()
