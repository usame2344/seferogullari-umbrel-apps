#!/bin/bash
# SPDX-License-Identifier: GPL-3.0
# scripts/fuzz_suite.sh
# Comprehensive Stratum fuzz suite for mkpool.
#
# Categories:
#   1. malformed-json        - invalid JSON / wrong types in standard fields
#   2. protocol-abuse        - semantically valid JSON, illegal at the Stratum layer
#   3. share-spam            - submit floods, replay, no-auth submits
#   4. auth-abuse            - bogus addresses, oversized worker names, control chars
#   5. slowloris             - drip-fed bytes, never-newline, oversize line
#   6. version-rolling-abuse - out-of-mask version bits, wrong configure shapes
#   7. binary-noise          - random bytes / utf-8 / NULs / huge payloads
#
# Hard requirement: the daemon MUST survive every category (same PID before/after)
# and MUST NOT log any handler exception that escapes the dispatch try/catch.
set -u
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-3331}"
UNIT="${UNIT:-mkpool-run}"
PROC_PATTERN="${PROC_PATTERN:-./build/mkpool}"
LOG="${LOG:-/var/log/mkpool/console.log}"

pid_of() { pgrep -f "$PROC_PATTERN" | head -1; }

assert_alive() {
    local label="$1"
    local before_pid="$2"
    sleep 1
    local now_pid; now_pid=$(pid_of)
    if [ -z "$now_pid" ] || [ "$now_pid" != "$before_pid" ]; then
        echo "FAIL [$label]: daemon died (before=$before_pid after=$now_pid)"
        return 1
    fi
    echo "OK   [$label]: pid=$now_pid"
}

fire() {
    local label="$1"; shift
    local payload="$1"; shift
    local before_pid; before_pid=$(pid_of)
    printf '%b' "$payload" | timeout 3 nc -q1 "$HOST" "$PORT" >/dev/null 2>&1 || true
    assert_alive "$label" "$before_pid"
}

fire_raw() {
    # fire_raw label  bytes - sends raw without newline.
    fire "$@"
}

# ---------------------------------------------------------------------------
# Pre-flight
# ---------------------------------------------------------------------------
echo "=== mkpool fuzz suite - host=$HOST port=$PORT unit=$UNIT ==="
START_PID=$(pid_of)
if [ -z "$START_PID" ]; then echo "FAIL: mkpool not running"; exit 1; fi
echo "start_pid=$START_PID"
LOG_OFF_START=$(wc -c < "$LOG" 2>/dev/null || echo 0)
FAILS=0

# ---------------------------------------------------------------------------
# 1. malformed-json
# ---------------------------------------------------------------------------
echo "--- 1. malformed-json"
fire json/empty            ''                                         || ((FAILS++))
fire json/blank-line       '\n\n\n'                                   || ((FAILS++))
fire json/garbage          'this is not json at all\n'                || ((FAILS++))
fire json/half-object      '{"id":1,"method":\n'                      || ((FAILS++))
fire json/unbalanced       '{"id":1,"method":"mining.subscribe"\n'    || ((FAILS++))
fire json/unicode-bom      '\xef\xbb\xbf{"id":1,"method":"mining.subscribe"}\n' || ((FAILS++))
fire json/nul-byte         '{"id":1,"method":"mining.subscribe\x00"}\n' || ((FAILS++))
fire json/cr-only          '{"id":1,"method":"mining.subscribe"}\r'   || ((FAILS++))

# ---------------------------------------------------------------------------
# 2. protocol-abuse - id types, missing fields, wrong shapes
# ---------------------------------------------------------------------------
echo "--- 2. protocol-abuse"
fire id/string             '{"id":"abc","method":"mining.subscribe"}\n'                          || ((FAILS++))
fire id/object             '{"id":{"x":1},"method":"mining.configure","params":[[],{}]}\n'      || ((FAILS++))
fire id/array              '{"id":[1,2],"method":"mining.subscribe"}\n'                         || ((FAILS++))
fire id/null               '{"id":null,"method":"mining.subscribe"}\n'                          || ((FAILS++))
fire id/float              '{"id":3.14,"method":"mining.subscribe"}\n'                          || ((FAILS++))
fire id/negative           '{"id":-99,"method":"mining.subscribe"}\n'                           || ((FAILS++))
fire id/huge-int           '{"id":999999999999999999999,"method":"mining.subscribe"}\n'         || ((FAILS++))
fire method/missing        '{"id":1,"params":[]}\n'                                             || ((FAILS++))
fire method/number         '{"id":1,"method":42}\n'                                             || ((FAILS++))
fire method/empty          '{"id":1,"method":""}\n'                                             || ((FAILS++))
fire method/unknown        '{"id":1,"method":"mining.nuke_everything"}\n'                       || ((FAILS++))
fire params/string         '{"id":1,"method":"mining.submit","params":"oops"}\n'                || ((FAILS++))
fire params/object         '{"id":1,"method":"mining.submit","params":{"w":"x"}}\n'             || ((FAILS++))
fire params/null           '{"id":1,"method":"mining.submit","params":null}\n'                  || ((FAILS++))
fire params/empty-array    '{"id":1,"method":"mining.submit","params":[]}\n'                    || ((FAILS++))
fire params/wrong-count    '{"id":1,"method":"mining.submit","params":[1,2]}\n'                 || ((FAILS++))
fire configure/wrong-shape '{"id":1,"method":"mining.configure","params":"bad"}\n'              || ((FAILS++))
fire configure/null-mask   '{"id":1,"method":"mining.configure","params":[["version-rolling"],{"version-rolling.mask":null}]}\n' || ((FAILS++))
fire configure/giant-mask  '{"id":1,"method":"mining.configure","params":[["version-rolling"],{"version-rolling.mask":"ffffffffffffffff"}]}\n' || ((FAILS++))
fire suggest/string        '{"id":1,"method":"mining.suggest_difficulty","params":["abc"]}\n'   || ((FAILS++))
fire suggest/nan           '{"id":1,"method":"mining.suggest_difficulty","params":[null]}\n'    || ((FAILS++))
fire suggest/negative      '{"id":1,"method":"mining.suggest_difficulty","params":[-1.0]}\n'    || ((FAILS++))
fire suggest/inf           '{"id":1,"method":"mining.suggest_difficulty","params":[1e400]}\n'   || ((FAILS++))
fire suggest/huge          '{"id":1,"method":"mining.suggest_difficulty","params":[1e308]}\n'   || ((FAILS++))

# ---------------------------------------------------------------------------
# 3. share-spam - submits without subscribe/auth
# ---------------------------------------------------------------------------
echo "--- 3. share-spam"
fire submit/no-auth        '{"id":1,"method":"mining.submit","params":["w","j","ee","ntime","nonce"]}\n' || ((FAILS++))
fire submit/bad-hex        '{"id":1,"method":"mining.submit","params":["w","j","zz","XXXX","YYYY"]}\n'  || ((FAILS++))
fire submit/short-ntime    '{"id":1,"method":"mining.submit","params":["w","j","ee","ab","cd"]}\n'     || ((FAILS++))
fire submit/long-nonce     '{"id":1,"method":"mining.submit","params":["w","j","ee","aabbccdd","aabbccddeeff00112233"]}\n' || ((FAILS++))
fire submit/replay         '{"id":1,"method":"mining.submit","params":["w","aabbccdd","00000000","60000000","12345678"]}\n{"id":2,"method":"mining.submit","params":["w","aabbccdd","00000000","60000000","12345678"]}\n' || ((FAILS++))
# 50 submits in one frame
BURST=$(python3 -c 'import sys; sys.stdout.write("".join(("{\"id\":%d,\"method\":\"mining.submit\",\"params\":[\"w\",\"j\",\"ee\",\"60000000\",\"12345678\"]}\n"%i) for i in range(50)))')
fire submit/burst50        "$BURST"                                                                      || ((FAILS++))

# ---------------------------------------------------------------------------
# 4. auth-abuse - oversized / weird worker strings
# ---------------------------------------------------------------------------
echo "--- 4. auth-abuse"
LONG_WORKER=$(python3 -c 'print("x"*4096)')
fire auth/long-worker      "{\"id\":1,\"method\":\"mining.authorize\",\"params\":[\"$LONG_WORKER\",\"x\"]}\n"        || ((FAILS++))
fire auth/embedded-quote   '{"id":1,"method":"mining.authorize","params":["a\"b.c","x"]}\n'                          || ((FAILS++))
fire auth/embedded-backslash '{"id":1,"method":"mining.authorize","params":["a\\b.c","x"]}\n'                        || ((FAILS++))
fire auth/control-chars    '{"id":1,"method":"mining.authorize","params":["a\u0000b\u0001c","x"]}\n'                 || ((FAILS++))
fire auth/numeric-user     '{"id":1,"method":"mining.authorize","params":[12345,"x"]}\n'                             || ((FAILS++))
fire auth/array-user       '{"id":1,"method":"mining.authorize","params":[["a","b"],"x"]}\n'                         || ((FAILS++))
fire auth/no-dot           '{"id":1,"method":"mining.authorize","params":["nodotjustsomestring","x"]}\n'             || ((FAILS++))
fire auth/bad-addr         '{"id":1,"method":"mining.authorize","params":["1invalidaddress.rig1","x"]}\n'            || ((FAILS++))
fire auth/empty-addr       '{"id":1,"method":"mining.authorize","params":[".rig1","x"]}\n'                           || ((FAILS++))
fire auth/utf8             '{"id":1,"method":"mining.authorize","params":["💩.rig1","x"]}\n'                          || ((FAILS++))

# ---------------------------------------------------------------------------
# 5. slowloris - line never completes
# ---------------------------------------------------------------------------
echo "--- 5. slowloris"
{
    before=$(pid_of)
    # Open 8 connections, dribble bytes, never close
    for i in 1 2 3 4 5 6 7 8; do
        (printf '{"id":1,"method"' ; sleep 6 ; printf ':"mining.subscribe"}\n') | timeout 8 nc -q1 "$HOST" "$PORT" >/dev/null 2>&1 &
    done
    sleep 9
    assert_alive slowloris/dribble "$before"
} || ((FAILS++))

# oversize single line (over the 1 MiB cap)
BIG=$(python3 -c 'print("x"*(2*1024*1024))')
fire slowloris/oversize    "{\"id\":1,\"method\":\"$BIG\"}\n"                                       || ((FAILS++))

# many small frames in one connection
MANY=$(python3 -c 'print("\n".join("{\"id\":%d,\"method\":\"mining.subscribe\"}"%i for i in range(200)))')
fire slowloris/200x-pipeline "$MANY"                                                                || ((FAILS++))

# ---------------------------------------------------------------------------
# 6. version-rolling abuse
# ---------------------------------------------------------------------------
echo "--- 6. version-rolling-abuse"
fire vr/non-string-mask    '{"id":1,"method":"mining.configure","params":[["version-rolling"],{"version-rolling.mask":42}]}\n' || ((FAILS++))
fire vr/short-mask         '{"id":1,"method":"mining.configure","params":[["version-rolling"],{"version-rolling.mask":"ff"}]}\n' || ((FAILS++))
fire vr/non-hex-mask       '{"id":1,"method":"mining.configure","params":[["version-rolling"],{"version-rolling.mask":"zzzzzzzz"}]}\n' || ((FAILS++))
fire vr/extension-array    '{"id":1,"method":"mining.configure","params":[42,{}]}\n'                                            || ((FAILS++))
fire vr/null-extensions    '{"id":1,"method":"mining.configure","params":[null,null]}\n'                                        || ((FAILS++))

# ---------------------------------------------------------------------------
# 7. binary-noise
# ---------------------------------------------------------------------------
echo "--- 7. binary-noise"
fire bin/random-1k         "$(head -c 1024 /dev/urandom | base64 -w0)\n"                            || ((FAILS++))
fire bin/all-nulls         "$(python3 -c 'import sys; sys.stdout.buffer.write(b"\x00"*256+b"\n")')"  || ((FAILS++))
fire bin/all-newlines      "$(python3 -c 'print("\n"*1000)')"                                       || ((FAILS++))
fire bin/high-bytes        "$(python3 -c 'import sys; sys.stdout.buffer.write(bytes(range(128,255))+b"\n")')" || ((FAILS++))

# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------
sleep 2
END_PID=$(pid_of)
echo
echo "=== verdict ==="
echo "start_pid=$START_PID end_pid=$END_PID failures=$FAILS"
if [ -n "$END_PID" ] && [ "$END_PID" = "$START_PID" ] && [ "$FAILS" -eq 0 ]; then
    echo "PASS: daemon survived all categories"
else
    echo "FAIL: see above"
fi

# Show any handler-level errors logged.
LOG_OFF_END=$(wc -c < "$LOG" 2>/dev/null || echo 0)
DELTA=$((LOG_OFF_END - LOG_OFF_START))
if [ "$DELTA" -gt 0 ]; then
    echo "=== new log lines mentioning errors/exceptions ==="
    tail -c "$DELTA" "$LOG" | grep -iE 'handler exception|warning|error|reject|disconnect|closing' | head -50 || true
fi
exit $((FAILS > 0))
