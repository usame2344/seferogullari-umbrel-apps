#!/bin/bash
# SPDX-License-Identifier: GPL-3.0
# scripts/fuzz_stratum.sh
# Fire a battery of malformed Stratum frames at the daemon. The daemon MUST
# stay alive (pid unchanged) and MUST respond with a well-formed error frame
# to every input.
set -u
HOST=127.0.0.1
PORT=3331

cat <<'EOF' > /tmp/fuzz_payloads.txt
{"id":"strid","method":"mining.subscribe"}
{"id":1,"method":"mining.authorize","params":[123]}
{"id":null,"method":"mining.submit","params":"notarray"}
{"id":{},"method":"mining.configure","params":"bad"}
{"id":3.14,"method":"mining.suggest_difficulty","params":["notnum"]}
notjsonatall
{"method":42}
{}
{"id":1,"method":"mining.subscribe","params":[12345]}
{"id":2,"method":"mining.authorize","params":[null]}
{"id":[],"method":"mining.submit","params":[]}
{"id":1,"method":"mining.submit","params":[1,2,3,4,5]}
{"id":1,"method":"mining.configure","params":[["version-rolling"],{"version-rolling.mask":42}]}
EOF

echo "=== BEFORE ==="
systemctl is-active mkpool-run
PID_BEFORE=$(pgrep -f './build/mkpool' | head -1)
echo "pid=$PID_BEFORE"

echo "=== FUZZ ==="
while IFS= read -r line; do
  printf '%s\n' "$line" | timeout 2 nc -q1 "$HOST" "$PORT" | head -1
done < /tmp/fuzz_payloads.txt

echo "=== AFTER ==="
sleep 2
systemctl is-active mkpool-run
PID_AFTER=$(pgrep -f './build/mkpool' | head -1)
echo "pid=$PID_AFTER"

if [ "$PID_BEFORE" = "$PID_AFTER" ] && [ -n "$PID_AFTER" ]; then
  echo "VERDICT: SURVIVED ($PID_AFTER)"
else
  echo "VERDICT: CRASHED (before=$PID_BEFORE after=$PID_AFTER)"
fi

echo "=== LOG (handler exceptions) ==="
grep -E 'handler exception|malformed request|parse error|reject auth|missing worker' /var/log/mkpool/console.log | tail -15
