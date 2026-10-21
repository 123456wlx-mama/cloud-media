#!/usr/bin/env bash
# 停止 start_server.sh -d 起的后台服务端
cd "$(dirname "$0")"

PIDFILE=build-gmake/server.pid
if [ ! -f "$PIDFILE" ]; then
    echo "没有找到 $PIDFILE（可能不是后台启动的）"
    pgrep -af cloud-media-server | head -5
    exit 0
fi

PID=$(cat "$PIDFILE")
if kill -0 "$PID" 2>/dev/null; then
    kill "$PID"
    echo "已停止 cloud-media-server (pid=$PID)"
else
    echo "进程 $PID 已经不在了"
fi
rm -f "$PIDFILE"
