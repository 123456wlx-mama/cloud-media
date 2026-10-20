#!/usr/bin/env bash
# Phase 7 before 基线：按矩阵跑 bencher，边跑边采服务端 /proc 指标。
#
# 每档：wipe → 采 5s 空载 → run（warmup + duration）× runs → 停采。
# 原始 CSV 落 bench_before/，汇总行落 bench_before/summary.txt。
#
# sysstat 未安装且环境策略禁止联网，pidstat/iostat 用 /proc 等价采样替代：
#   cpu_pct  = Δ(utime+stime)/HZ/interval
#   rss_kb   = VmRSS
#   wr_kb    = Δ(/proc/diskstats sda sectors_written)*512/1024
set -uo pipefail

ROOT="${BENCH_ROOT:-$HOME/cloud-media}"
BIN="${BENCH_BIN:-$ROOT/build-bench/bin}"
DATA="${BENCH_DATA:-$HOME/cm_bench_data}"
OUT="${BENCH_OUT:-$ROOT/bench_before}"
PORT="${BENCH_PORT:-8014}"
CSV="$OUT/before.csv"
SUMMARY="$OUT/summary.txt"

source "$HOME/cm_bench_env.sh"
mkdir -p "$OUT"
: > "$SUMMARY"

HZ="$(getconf CLK_TCK)"
PIDF="$DATA/server.pid"
STOPFLAG="$OUT/.sampler.stop"
SAMPLER_PID=""

cpu_ticks() {
    awk '{print $14+$15}' "/proc/$1/stat"
}

rss_kb() {
    awk '/VmRSS/ {print $2}' "/proc/$1/status"
}

disk_written_sectors() {
    awk '$3=="sda" {print $10}' /proc/diskstats
}

sample_loop() {
    local tag="$1" pid prev_ticks prev_wr prev_t now ticks wr rss cpu
    pid="$(cat "$PIDF")"
    prev_ticks="$(cpu_ticks "$pid")"
    prev_wr="$(disk_written_sectors)"
    prev_t="$(date +%s)"
    while [ ! -f "$STOPFLAG" ]; do
        sleep 1
        [ -d "/proc/$pid" ] || break
        now="$(date +%s)"
        ticks="$(cpu_ticks "$pid")"
        wr="$(disk_written_sectors)"
        rss="$(rss_kb "$pid")"
        cpu="$(awk -v a="$ticks" -v b="$prev_ticks" -v hz="$HZ" -v dt="$((now - prev_t))" \
            'BEGIN { if (dt <= 0) dt = 1; printf "%.1f", (a-b)*100.0/hz/dt }')"
        local wr_kb=$(( (wr - prev_wr) * 512 / 1024 ))
        echo "$((now - start_t)),$tag,$cpu,$rss,$wr_kb"
        prev_ticks="$ticks"; prev_wr="$wr"; prev_t="$now"
    done
}

start_sampler() {
    local tag="$1"
    rm -f "$STOPFLAG"
    start_t="$(date +%s)"
    echo "t_s,cell,cpu_pct,rss_kb,disk_write_kb" > "$OUT/sys_$tag.csv"
    sample_loop "$tag" >> "$OUT/sys_$tag.csv" &
    SAMPLER_PID=$!
}

stop_sampler() {
    touch "$STOPFLAG"
    [ -n "$SAMPLER_PID" ] && wait "$SAMPLER_PID" 2>/dev/null
    SAMPLER_PID=""
}

run_cell() {
    local tag="$1"; shift
    echo "=== $tag ==="
    bash "$ROOT/scripts/bench_env.sh" wipe >/dev/null
    start_sampler "$tag"
    "$BIN/bencher" -p "$PORT" --csv "$CSV" "$@" 2>&1 | tee "$OUT/$tag.log" | grep -E '^\[run|^SUMMARY|setup:|errors'
    stop_sampler
    grep '^SUMMARY' "$OUT/$tag.log" | sed "s/^/$tag /" >> "$SUMMARY"
    echo
}

run_cell upload10_c100    -op upload -c 100  -t 4 -size 10 -duration 15 -warmup 5 -runs 3 --inflight 64
run_cell upload10_c1000   -op upload -c 1000 -t 4 -size 10 -duration 15 -warmup 5 -runs 3 --inflight 64
run_cell upload10_c3000   -op upload -c 3000 -t 4 -size 10 -duration 15 -warmup 8 -runs 3 --inflight 64
run_cell upload1_c100     -op upload -c 100  -t 4 -size 1  -duration 15 -warmup 5 -runs 3 --inflight 64
run_cell upload1_c1000    -op upload -c 1000 -t 4 -size 1  -duration 15 -warmup 8 -runs 3 --inflight 64
run_cell download10_c100  -op download -c 100  -t 4 -size 10 -duration 15 -warmup 5 -runs 3 --setup-files 4 --inflight 64
run_cell download10_c1000 -op download -c 1000 -t 4 -size 10 -duration 15 -warmup 8 -runs 3 --setup-files 4 --inflight 64
run_cell list_c1000       -op list -c 1000 -t 4 -duration 60 -warmup 10 -runs 3

echo "=== connect storm c=8000 ==="
bash "$ROOT/scripts/bench_env.sh" wipe >/dev/null
start_sampler storm8000
"$BIN/bencher" -p "$PORT" -op connect -c 8000 -t 4 --csv "$CSV" 2>&1 | tee "$OUT/storm8000.log" | grep -E '^\[storm'
stop_sampler
grep '^\[storm' "$OUT/storm8000.log" | sed 's/^/storm8000 /' >> "$SUMMARY"

echo
echo "=== summary ==="
cat "$SUMMARY"
echo "raw: $CSV  sys: $OUT/sys_*.csv"
