#!/usr/bin/env bash
# 汇总 bench_before/sys_*.csv：CPU 均值/峰值、RSS 区间、磁盘写均值/峰值
set -uo pipefail
cd "$HOME/cloud-media/bench_before" || exit 1
for f in sys_*.csv; do
    cell="${f#sys_}"
    cell="${cell%.csv}"
    awk -F, -v cell="$cell" '
        FNR == 1 { next }
        {
            n++
            cpu += $3; if ($3 > cpu_max) cpu_max = $3
            if (n == 1 || $4 < rss_min) rss_min = $4
            if ($4 > rss_max) rss_max = $4
            wr += $5; if ($5 > wr_max) wr_max = $5
        }
        END {
            if (n == 0) { printf "%-18s no-samples\n", cell; exit }
            printf "%-18s n=%-3d cpu_avg=%5.0f%% cpu_max=%5.0f%% rss=%4d..%4dMB wr_avg=%6.0fMB/s wr_max=%6.0fMB/s\n",
                cell, n, cpu / n, cpu_max, rss_min / 1024, rss_max / 1024, wr / n / 1024, wr_max / 1024
        }' "$f"
done
