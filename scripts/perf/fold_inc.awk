#!/bin/awk -f
BEGIN {
    n = split("completeUpload mergeChunks writeBlockCrc UploadChunkHandler verifyFrameCrc handleUploadChunk ConnectionDecoder EpollPoller ThreadPool MysqlConn FileService TransferDao sendfile", pats, " ")
}
{
    cnt = $NF / 1000000
    stack = $0
    sub(/ [0-9]+$/, "", stack)
    total += cnt
    for (i = 1; i <= n; i++) {
        if (index(stack, pats[i]) > 0) hit[pats[i]] += cnt
    }
}
END {
    printf "total_samples=%.0f\n", total
    for (i = 1; i <= n; i++) {
        p = pats[i]
        printf "%-22s %7.2f%%\n", p, (total ? 100.0 * hit[p] / total : 0)
    }
}
