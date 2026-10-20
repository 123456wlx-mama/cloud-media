#!/bin/bash
# perf 采样：起压测 → 稳态 → perf record -p server → 报告 + 火焰图
# usage: perf_run.sh <tag> <bencher args...>
set -u
TAG="$1"; shift
ROOT=/home/colin/cloud-media
BIN=$ROOT/build-bench/bin/bencher
PERF=/usr/lib/linux-tools/4.4.0-210-generic/perf
FG=/home/colin/FlameGraph
OUT=/home/colin/perf_out
source /home/colin/cm_bench_env.sh
mkdir -p "$OUT"

SPID="$(cat /home/colin/cm_bench_data/server.pid)"
echo "server pid=$SPID tag=$TAG"

bash "$ROOT/scripts/bench_env.sh" wipe >/dev/null
# 脚本以 root 跑（perf_event_paranoid=3），wipe 后必须把数据目录还给服务端用户
chown -R colin:colin /home/colin/cm_bench_data

"$BIN" -p 8014 "$@" --csv "$OUT/$TAG.csv" > "$OUT/$TAG.log" 2>&1 &
BPID=$!

# 等稳态：warmup + 一点余量
sleep 20

echo "--- perf record 30s ---"
"$PERF" record -F 99 -g -p "$SPID" -o "$OUT/$TAG.data" -- sleep 30 2>&1 | tail -2

wait "$BPID" 2>/dev/null

echo "--- perf report ---"
"$PERF" report -i "$OUT/$TAG.data" --stdio --no-children -g none 2>/dev/null \
    | grep -E "^ +[0-9]" | head -40 > "$OUT/$TAG.top.txt"
"$PERF" report -i "$OUT/$TAG.data" --stdio --no-children -g none 2>/dev/null \
    | head -30 > "$OUT/$TAG.report.txt"

echo "--- folded stacks ---"
"$PERF" script -i "$OUT/$TAG.data" 2>/dev/null > "$OUT/$TAG.script"
"$FG/stackcollapse-perf.pl" "$OUT/$TAG.script" > "$OUT/$TAG.folded" 2>/dev/null

echo "--- flamegraph ---"
"$FG/flamegraph.pl" --title "$TAG" --width 1400 "$OUT/$TAG.folded" > "$OUT/$TAG.svg"
ls -la "$OUT/$TAG.svg" "$OUT/$TAG.folded" "$OUT/$TAG.top.txt"
grep "^SUMMARY" "$OUT/$TAG.log"
