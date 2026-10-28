#!/usr/bin/env bash
# End-to-end self test against a local range-capable server.
# Verifies byte-exact assembly, resume, and connection scaling behaviour.
set -u
BIN=${MTDL_BIN:-build/mtdl}
[ -x "$BIN" ] || { echo "build first (or set MTDL_BIN)"; exit 1; }
BIN=$(readlink -f "$BIN")

WORK=$(mktemp -d)
PORT=${PORT:-8809}
SRC=$WORK/source.bin
trap 'kill ${SRV:-0} ${SRV2:-0} ${SRV3:-0} 2>/dev/null; rm -rf "$WORK"' EXIT

echo "== creating 64 MiB test payload"
head -c $((64*1024*1024)) /dev/urandom > "$SRC"
SUM=$(sha256sum "$SRC" | cut -d' ' -f1)

python3 "$(dirname "$0")/testsrv.py" "$SRC" "$PORT" 0 > "$WORK/srv.log" 2>&1 &
SRV=$!
sleep 0.7

check() {  # check <file> <label>
  if [ "$(sha256sum "$1" | cut -d' ' -f1)" = "$SUM" ]; then
    echo "   OK   byte-exact: $2"
  else
    echo "   FAIL byte-exact: $2"; FAILED=1
  fi
}
FAILED=0

echo "== full download, 1 connection"
"$BIN" "http://127.0.0.1:$PORT/source.bin" -c 1 --no-adaptive -o "$WORK/a.bin" -q \
  && check "$WORK/a.bin" "1 conn"

echo "== full download, 32 connections, 512 KiB chunks"
"$BIN" "http://127.0.0.1:$PORT/source.bin" -c 32 --no-adaptive -s 512KiB -o "$WORK/b.bin" -q \
  && check "$WORK/b.bin" "32 conns"

echo "== full download, adaptive ramp 4 -> 16"
"$BIN" "http://127.0.0.1:$PORT/source.bin" -c 16 --min-connections 4 -o "$WORK/c.bin" -q \
  && check "$WORK/c.bin" "adaptive"

echo "== interrupt plan: rate-limited server so Ctrl-C lands mid-download"
PORT2=$((PORT+1))
python3 "$(dirname "$0")/testsrv.py" "$SRC" "$PORT2" 1500000 > "$WORK/srv2.log" 2>&1 &
SRV2=$!
sleep 0.7
URL2="http://127.0.0.1:$PORT2/source.bin"

echo "== interrupt at ~30%, resume, verify"
"$BIN" "$URL2" -c 8 --no-adaptive -o "$WORK/d.bin" -q &
PID=$!
sleep 0.9
kill -INT "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
if [ ! -f "$WORK/d.bin.mtdl-state" ]; then
  echo "   FAIL no resume state written"; FAILED=1
else
  BEFORE=$(grep -o 'done .*' "$WORK/d.bin.mtdl-state" | cut -c6-14)
  echo "   partial state: $BEFORE..."
  "$BIN" "$URL2" -c 8 -o "$WORK/d.bin" -q && check "$WORK/d.bin" "resume"
fi

echo "== interrupt mid-block on a trickling server (partial writes), resume"
PORT3=$((PORT+2))
python3 "$(dirname "$0")/testsrv.py" "$SRC" "$PORT3" 900000 > "$WORK/srv3.log" 2>&1 &
SRV3=$!
sleep 0.7
"$BIN" "http://127.0.0.1:$PORT3/source.bin" -c 6 --no-adaptive -o "$WORK/h.bin" -q &
PID=$!
sleep 1.5
kill -INT "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
kill "$SRV3" 2>/dev/null
if [ ! -f "$WORK/h.bin.mtdl-state" ]; then
  echo "   FAIL no resume state written"; FAILED=1
else
  "$BIN" "http://127.0.0.1:$PORT3/source.bin" -c 6 -o "$WORK/h.bin" -q \
    && check "$WORK/h.bin" "partial-block resume"
fi
kill "$SRV2" 2>/dev/null

echo "== fan-out over three edges (127.0.0.1..3), 12 connections"
"$BIN" "http://127.0.0.1:$PORT/source.bin" -c 12 --no-adaptive \
  --endpoints 127.0.0.1,127.0.0.2,127.0.0.3 -o "$WORK/f.bin" -q \
  && check "$WORK/f.bin" "fan-out"
grep -q "edges      : 3" <("$BIN" "http://127.0.0.1:$PORT/source.bin" -c 4 --no-adaptive \
  --endpoints 127.0.0.1,127.0.0.2,127.0.0.3 -o "$WORK/f2.bin" 2>/dev/null) \
  && echo "   OK   three edges used" || { echo "   FAIL edge count"; FAILED=1; }

echo "== dead edge is retired, download still completes"
"$BIN" "http://127.0.0.1:$PORT/source.bin" -c 8 --no-adaptive -s 1MiB \
  --endpoints 127.0.0.99,127.0.0.1 -o "$WORK/g.bin" -q \
  && check "$WORK/g.bin" "dead edge skipped"

echo "== error handling: missing file must fail cleanly"
if "$BIN" "http://127.0.0.1:$PORT/nope.bin" -o "$WORK/e.bin" -q 2>/dev/null; then
  echo "   FAIL expected failure for 404"; FAILED=1
else
  echo "   OK   404 rejected"
fi

echo
if [ "$FAILED" -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "SOME TESTS FAILED"; fi
exit "$FAILED"
