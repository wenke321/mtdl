#!/usr/bin/env bash
# Benchmark mtdl against a URL.
#
# The interesting knob on a CDN/mirror is not the connection count but the
# number of *edges* used, so this runs the same client in both modes:
#   single edge  (--no-fanout)  -> baseline, what a plain HTTP client sees
#   fan-out      (default)      -> spread over every resolved address
#
# Usage: scripts/bench.sh <url> [outdir] [conns...]
set -u
URL=${1:?usage: bench.sh <url> [outdir] [conns...]}
OUT=${2:-/tmp/mtdl-bench}
shift 2 2>/dev/null || true
CONNS=("$@")
[ ${#CONNS[@]} -eq 0 ] && CONNS=(32 64 96)

BIN=${MTDL_BIN:-build/mtdl}
[ -x "$BIN" ] || { echo "cannot find $BIN (build first, or set MTDL_BIN)"; exit 1; }
mkdir -p "$OUT"

run() {  # run <label> <args...>
  local label=$1; shift
  rm -f "$OUT/bench.bin"*
  local start end wall out speed conns edges
  start=$(date +%s.%N)
  out=$("$BIN" "$URL" --no-resume -o "$OUT/bench.bin" "$@" 2>&1)
  end=$(date +%s.%N)
  wall=$(awk -v a="$start" -v b="$end" 'BEGIN{printf "%.2f", b-a}')
  speed=$(printf '%s\n' "$out" | sed -n 's/^avg speed  : \([0-9.]*\).*/\1/p' | tail -1)
  conns=$(printf '%s\n' "$out" | sed -n 's/^connections: //p' | tail -1)
  edges=$(printf '%s\n' "$out" | sed -n 's/^edges      : //p' | tail -1)
  printf '%-14s %-8s %-7s %-10s %s\n' "$label" "${conns:--}" "${edges:--}" "${speed:--}" "$wall"
}

printf '%-14s %-8s %-7s %-10s %s\n' mode conns edges MiB/s wall_s
for c in "${CONNS[@]}"; do
  run "single-edge" --no-fanout --no-adaptive -c "$c"
done
for c in "${CONNS[@]}"; do
  run "fan-out" --no-adaptive -c "$c"
done
rm -f "$OUT/bench.bin"*
