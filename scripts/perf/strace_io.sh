#!/bin/bash
# 写放大分段：strace -y 按 fd 路径统计 6s 内的 read/write 字节
set -u
ROOT=/home/colin/cloud-media
BIN=$ROOT/build-bench/bin/bencher
source /home/colin/cm_bench_env.sh

bash "$ROOT/scripts/bench_env.sh" wipe >/dev/null
chown -R colin:colin /home/colin/cm_bench_data
SPID="$(cat /home/colin/cm_bench_data/server.pid)"

"$BIN" -p 8014 -op upload -c 100 -t 4 -size 10 -duration 45 -warmup 10 --inflight 64 \
    > /tmp/st_bench.log 2>&1 &
BPID=$!
sleep 22

echo "--- strace 6s ---"
timeout 6 strace -f -y -s 0 -e trace=write,read,pwrite64,pread64 -p "$SPID" -o /tmp/st.log
echo "strace log lines: $(wc -l < /tmp/st.log)"

wait "$BPID"

sed -n "s/.*<\([^>]*\)>.*= \([0-9-]*\)$/\1 \2/p" /tmp/st.log > /tmp/st.parsed
awk '
{
    path = $1; n = $2
    if (n <= 0) next
    if (path ~ /cm_bench_data\/tmp/)        cat = "a_tmp"
    else if (path ~ /cm_bench_data\/blobs/) cat = "c_blob"
    else if (path ~ /TCP|socket|UNIX/)      cat = "net"
    else if (path ~ /\.log/)                cat = "log"
    else                                    cat = "other"
    bytes[cat] += n
    cnt[cat] += 1
}
END {
    printf "%-8s %14s %10s %10s\n", "cat", "bytes/6s", "MB/s", "calls"
    for (c in bytes) printf "%-8s %14d %10.2f %10d\n", c, bytes[c], bytes[c]/6/1048576, cnt[c]
}' /tmp/st.parsed | sort -k2 -nr

echo "--- 明细：写 4MB 以上的调用按路径前缀 ---"
sed -n "s/.*<\([^>]*\)>.*= \([0-9-]*\)$/\1 \2/p" /tmp/st.log \
    | awk '$2 > 1048576 {print $1}' | sed "s|/home/colin/cm_bench_data/||" | cut -d/ -f1 | sort | uniq -c
grep "^SUMMARY" /tmp/st_bench.log
