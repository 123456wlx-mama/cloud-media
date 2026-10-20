#!/bin/bash
# 清态协议：wipe(库+blob) → 归还属主 → drop_caches → 记录 df
set -u
bash /home/colin/cloud-media/scripts/bench_env.sh wipe
chown -R colin:colin /home/colin/cm_bench_data
sync
echo 3 > /proc/sys/vm/drop_caches
echo "--- after clean ---"
df -h / | tail -1
cat /proc/loadavg
