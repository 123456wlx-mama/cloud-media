#!/bin/awk -f
{
    line = $0
    sub(/^\[pid +[0-9]+\] +/, "", line)
    sub(/^[0-9]+ +/, "", line)
    sub(/^\[[^]]*\] +/, "", line)
    fn = line
    sub(/\(.*/, "", fn)
    path = "-"
    if (match(line, /<[^>]*>/)) {
        path = substr(line, RSTART + 1, RLENGTH - 2)
    }
    ret = line
    sub(/.*= /, "", ret)
    ret = ret + 0
    if (ret <= 0) next
    if (fn != "read" && fn != "write" && fn != "pread64" && fn != "pwrite64") next

    isRead = (fn == "read" || fn == "pread64")

    if (path ~ /merged\.bin\.tmp/)                  cat = "c_merge_write"
    else if (path ~ /cm_bench_data\/tmp/) {
        if (isRead) cat = "b_merge_read"; else cat = "a_chunk_write"
    }
    else if (path ~ /cm_bench_data\/blobs/)         cat = "d_blob_publish"
    else if (path ~ /TCP|socket/)                   cat = "net"
    else if (path ~ /\.log/)                        cat = "log"
    else                                            cat = "other"

    if (isRead) { rb[cat] += ret; rc[cat]++ } else { wb[cat] += ret; wc[cat]++ }
}
END {
    printf "%-16s %14s %8s %7s | %14s %8s %7s\n", "stage", "read B/6s", "MB/s", "calls", "write B/6s", "MB/s", "calls"
    split("a_chunk_write b_merge_read c_merge_write d_blob_publish log net other", order, " ")
    for (i = 1; i <= 7; i++) {
        c = order[i]
        if (rb[c] + wb[c] == 0) continue
        printf "%-16s %14d %8.2f %7d | %14d %8.2f %7d\n", c, rb[c], rb[c]/6/1048576, rc[c], wb[c], wb[c]/6/1048576, wc[c]
    }
}
