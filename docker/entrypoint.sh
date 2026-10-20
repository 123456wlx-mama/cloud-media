#!/bin/sh
# 数据卷首次挂载时把子目录建好，避免服务端因目录不存在退出
set -e
mkdir -p /data/blobs /data/downloads /data/logs
exec "$@"
