#!/bin/bash
# 规范化 perf report 输出：overhead / 共享对象 / 符号
P=/usr/lib/linux-tools/4.4.0-210-generic/perf
for t in "$@"; do
    echo "== $t =="
    $P report -i /home/colin/perf_out/$t.data --stdio --no-children -g none 2>/dev/null \
        | sed -e "s/^[[:space:]]*//" -e "s/[[:space:]][[:space:]]*/ /g" \
        | awk -F" " '$1 ~ /%$/ {printf "%-8s %-18s %s\n", $1, $(NF-1), $NF}' \
        | head -22
    echo
done
