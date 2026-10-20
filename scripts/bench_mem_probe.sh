#!/usr/bin/env bash
# 在途上限 → 常驻内存 的放大系数探针：同一 op 下改 --inflight，分别采服务端与客户端峰值 RSS。
set -uo pipefail
source "$HOME/cm_bench_env.sh"
ROOT="$HOME/cloud-media"
BIN="$ROOT/build-bench/bin/bencher"
DATA="$HOME/cm_bench_data"
OUT="$ROOT/bench_before"
mkdir -p "$OUT"

peek_rss() { awk "/VmRSS/ {print \$2}" "/proc/$1/status" 2>/dev/null; }

probe() {
    local inflight="$1" spid cpid srv_peak=0 cli_peak=0 srv cli
    bash "$ROOT/scripts/bench_env.sh" wipe >/dev/null
    spid="$(cat "$DATA/server.pid")"
    "$BIN" -p 8014 -op upload -c 100 -t 4 -size 10 -duration 15 -warmup 3 -runs 1 \
        --inflight "$inflight" > "$OUT/probe_inflight$inflight.log" 2>&1 &
    cpid=$!
    while kill -0 "$cpid" 2>/dev/null; do
        srv="$(peek_rss "$spid")"; cli="$(peek_rss "$cpid")"
        [ -n "$srv" ] && [ "$srv" -gt "$srv_peak" ] && srv_peak="$srv"
        [ -n "$cli" ] && [ "$cli" -gt "$cli_peak" ] && cli_peak="$cli"
        sleep 0.5
    done
    wait "$cpid"
    local qps
    qps="$(grep -o "file_qps=[0-9.]*" "$OUT/probe_inflight$inflight.log" | tail -1)"
    printf "inflight=%-4s server_peak=%4dMB client_peak=%4dMB server_per_slot=%4.1fMB %s\n" \
        "$inflight" "$((srv_peak / 1024))" "$((cli_peak / 1024))" \
        "$(awk -v a="$srv_peak" -v b="$inflight" "BEGIN{printf \"%.1f\", a/1024/b}")" "$qps"
}

echo "base: server idle RSS = $(peek_rss "$(cat "$DATA/server.pid")") kB"
for k in 32 64 128; do probe "$k"; done
