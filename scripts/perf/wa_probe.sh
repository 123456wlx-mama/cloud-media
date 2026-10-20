#!/bin/bash
# 写放大两口径：syscall 级（/proc/pid/io wchar）与设备级（/proc/diskstats），
# 外加 strace 15s 的分段归属（tmp 分片写 / merge 读 / merge 写 / blob 回读）。
set -u
ROOT=/home/colin/cloud-media
BIN=$ROOT/build-bench/bin/bencher
source /home/colin/cm_bench_env.sh
OUT=/home/colin/perf_out
mkdir -p "$OUT"

bash "$ROOT/scripts/bench_env.sh" wipe >/dev/null
chown -R colin:colin /home/colin/cm_bench_data
SPID="$(cat /home/colin/cm_bench_data/server.pid)"

"$BIN" -p 8014 -op upload -c 100 -t 4 -size 10 -duration 80 -warmup 10 --inflight 64 \
    > "$OUT/wa_bench.log" 2>&1 &
BPID=$!

sleep 20
echo "t,wchar_kb,disk_wr_kb" > "$OUT/io_sample.csv"
prev_w=$(awk "/^wchar:/ {print \$2}" /proc/$SPID/io)
prev_d=$(awk "\$3==\"sda\" {print \$10}" /proc/diskstats)
for i in $(seq 1 20); do
    sleep 1
    w=$(awk "/^wchar:/ {print \$2}" /proc/$SPID/io)
    d=$(awk "\$3==\"sda\" {print \$10}" /proc/diskstats)
    echo "$i,$(( (w - prev_w) / 1024 )),$(( (d - prev_d) * 512 / 1024 ))" >> "$OUT/io_sample.csv"
    prev_w=$w; prev_d=$d
done

echo "--- strace 15s ---"
timeout 15 strace -f -y -s 0 -e trace=write,read,pwrite64,pread64 -p "$SPID" -o "$OUT/wa_strace.log"
wait "$BPID"

echo "--- syscall vs 设备 ---"
awk -F, "NR>1 { w+=\$2; d+=\$3; n++ } END { printf \"平均 wchar=%.1f MB/s  设备写=%.1f MB/s  设备写/wchar=%.2f\n\", w/n/1024, d/n/1024, (w?d/w:0) }" "$OUT/io_sample.csv"
grep "^SUMMARY" "$OUT/wa_bench.log"

echo "--- strace 分段（%） ---"
awk -f /home/colin/st_agg.awk "$OUT/wa_strace.log"
