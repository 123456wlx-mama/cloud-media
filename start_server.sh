#!/usr/bin/env bash
# ============================================================================
#  cloud-media 服务端一键编译 + 启动（Linux，不依赖 CMake）
#
#  用法：
#      ./start_server.sh            # 默认 8004；被占用时自动换 8010
#      ./start_server.sh 8020       # 指定端口
#      ./start_server.sh 8020 -d     # 后台运行（日志写 build-gmake/server.log）
#
#  停止：前台按 Ctrl+C；后台用 ./stop_server.sh
# ============================================================================
set -e
cd "$(dirname "$0")"

PORT="${1:-8004}"
DAEMON=0
[ "$2" = "-d" ] && DAEMON=1

echo "==> [1/4] 编译服务端（增量，改过才编）"
make -C server -j"$(nproc)"

echo "==> [2/4] 检查端口 $PORT"
if ss -lnt 2>/dev/null | grep -q ":$PORT "; then
    NEW=$((PORT + 10))
    echo "    端口 $PORT 已被占用（可能是老服务端），自动改用 $NEW"
    PORT="$NEW"
fi

echo "==> [3/4] 准备配置"
mkdir -p build-gmake
CONF="build-gmake/server.conf"
if [ ! -f "$CONF" ]; then
    if [ -f build/e2e.conf ]; then
        sed "s/^listen_port.*/listen_port = $PORT/" build/e2e.conf > "$CONF"
    else
        sed -e "s/^listen_port.*/listen_port = $PORT/" \
            -e "s/^host = mysql/host = 127.0.0.1/" \
            -e "s/^host = redis/host = 127.0.0.1/" \
            docker/server.conf > "$CONF"
    fi
    echo "    已生成 $CONF（port=$PORT）"
else
    sed -i "s/^listen_port.*/listen_port = $PORT/" "$CONF"
fi

# 依赖自检：给出人话提示，而不是让服务端自己报连接错误
if ! redis-cli ping >/dev/null 2>&1; then
    echo "    !! Redis 没起：redis-server --daemonize yes --port 6379"
fi
if ! mysqladmin ping >/dev/null 2>&1; then
    echo "    !! MySQL 可能没起（若用了密码，这条 ping 会误报，可忽略）"
fi

echo "==> [4/4] 启动服务端（Ctrl+C 停止）"
echo "    客户端连接：--host 127.0.0.1 --port $PORT"
echo "----------------------------------------------------------------"

if [ "$DAEMON" = "1" ]; then
    nohup ./build-gmake/cloud-media-server -c "$CONF" > build-gmake/server.log 2>&1 &
    echo $! > build-gmake/server.pid
    sleep 2
    echo "后台运行 pid=$(cat build-gmake/server.pid)，日志：tail -f build-gmake/server.log"
else
    exec ./build-gmake/cloud-media-server -c "$CONF"
fi
