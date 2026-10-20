#!/bin/bash
# 读取侧：rchar（syscall 读）vs 块层读扇区，判断 merge 回读是否命中 page cache
set -u
ROOT=/home/colin/cloud-media
BIN=$ROOT/build-bench/bin/bencher
source /home/colin/cm_bench_env.sh
SPID="$(cat /home/colin/cm_bench_data/server.pid)"
bash "$ROOT/scripts/bench_env.sh" wipe >/dev/null

"$BIN" -p 8014 -op upload -c 100 -t 4 -size 10 -duration 30 -warmup 5 --inflight 64 > /tmp/rd_bench.log 2>&1 &
BPID=$!
sleep 12

pr=$(awk "/^rchar:/ {print \$2}" /proc/$SPID/io)
pw=$(awk "/^wchar:/ {print \$2}" /proc/$SPID/io)
dr=$(awk "\$3==\"sda\" {print \$6}" /proc/diskstats)
dw=$(awk "\$3==\"sda\" {print \$10}" /proc/diskstats)
sleep 10
pr2=$(awk "/^rchar:/ {print \$2}" /proc/$SPID/io)
pw2=$(awk "/^wchar:/ {print \$2}" /proc/$SPID/io)
dr2=$(awk "\$3==\"sda\" {print \$6}" /proc/diskstats)
dw2=$(awk "\$3==\"sda\" {print \$10}" /proc/diskstats)

awk -v pr=$pr -v pr2=$pr2 -v pw=$pw -v pw2=$pw2 -v dr=$dr -v dr2=$dr2 -v dw=$dw -v dw2=$dw2 "BEGIN {
  printf \"10s 窗口:\n\"
  printf \"  rchar  = %7.1f MB/s   (用户态读窗口)\n\", (pr2-pr)/10/1048576
  printf \"  wchar  = %7.1f MB/s   (用户态写)\n\", (pw2-pw)/10/1048576
  printf \"  块层读 = %7.1f MB/s   (设备实际读)\n\", (dr2-dr)*512/10/1048576
  printf \"  块层写 = %7.1f MB/s   (设备实际写)\n\", (dw2-dw)*512/10/1048576
}"
wait "$BPID"
grep "^SUMMARY" /tmp/rd_bench.log
