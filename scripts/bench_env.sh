#!/usr/bin/env bash
# Phase 7 压测环境：独立库 cloud_media_bench + 独立数据目录 + 端口 8014，
# 与 e2e（8004 / cloud_media_test）完全隔离，压测不需要停对方的服务端。
#
# 用法：bash scripts/bench_env.sh init|start|stop|wipe|status
#   init    建库建表 + 建压测账号 + 写 bench.conf（幂等）
#   start   拉起服务端（ulimit -n 65535）
#   stop    停服务端
#   wipe    清空压测库与 blob（每档之间保持干净）
#   status  端口/进程
#
# 口令：MySQL 口令从 build/e2e.conf 读取；bencher 账号口令写入 ~/cm_bench_env.sh（0600），
# 脚本与日志都不打印明文。
set -euo pipefail

ROOT="${BENCH_ROOT:-$HOME/cloud-media}"
BIN="${BENCH_BIN:-$ROOT/build-bench/bin}"
CONF="${BENCH_CONF:-$ROOT/bench.conf}"
DATA="${BENCH_DATA:-$HOME/cm_bench_data}"
PORT="${BENCH_PORT:-8014}"
DB="${BENCH_DB:-cloud_media_bench}"
TEL="${BENCH_TEL:-13900000007}"
ENVFILE="${BENCH_ENVFILE:-$HOME/cm_bench_env.sh}"
MYSQL_USER="${BENCH_MYSQL_USER:-cm_test}"

mysql_pw() {
    awk -F= '/^\[/{sec=$0} sec=="[mysql]" && $1=="password" {gsub(/[ \t]/,"",$2); print $2; exit}' \
        "$ROOT/build/e2e.conf"
}

sql() {
    MYSQL_PWD="$(mysql_pw)" mysql -N -B -u"$MYSQL_USER" "$DB" -e "$1"
}

do_init() {
    local dbpw pw salt hash
    dbpw="$(mysql_pw)"
    [ -n "$dbpw" ] || { echo "no mysql password in $ROOT/build/e2e.conf" >&2; exit 1; }
    mkdir -p "$DATA" "$DATA/downloads"
    MYSQL_PWD="$dbpw" mysql -u"$MYSQL_USER" \
        -e "CREATE DATABASE IF NOT EXISTS \`$DB\` DEFAULT CHARSET utf8mb4"
    MYSQL_PWD="$dbpw" mysql -u"$MYSQL_USER" "$DB" < "$ROOT/sql/schema_v2.sql"

    if [ ! -f "$ENVFILE" ]; then
        pw="$(head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n')"
        salt="$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
        printf 'export CM_CLI_PASSWORD=%s\nexport CM_CLI_TEL=%s\n' "$pw" "$TEL" > "$ENVFILE"
        chmod 600 "$ENVFILE"
        hash="$(printf '%s' "$salt$pw" | md5sum | cut -d' ' -f1)"
        MYSQL_PWD="$dbpw" mysql -u"$MYSQL_USER" "$DB" -e \
            "INSERT INTO t_user (u_tel,u_password,u_salt,u_pwd_algo,u_name,u_status)
             VALUES ('$TEL','$hash','$salt',1,'bench',0)
             ON DUPLICATE KEY UPDATE u_password=VALUES(u_password),
             u_salt=VALUES(u_salt), u_pwd_algo=1, u_status=0"
        echo "bench account created: tel=$TEL password -> $ENVFILE"
    fi

    cat > "$CONF" <<CONF
[server]
listen_ip=127.0.0.1
listen_port=$PORT
io_threads=2
worker_threads=4
data_dir=$DATA

[log]
path=$DATA/server.log
level=info
max_size_mb=200
roll_days=1

[mysql]
host=127.0.0.1
port=3306
user=$MYSQL_USER
password=$dbpw
database=$DB

[redis]
host=127.0.0.1
port=6379
db=1

[transfer]
download_dir=$DATA/downloads
max_connections_per_file=4
max_concurrent_files=2
CONF
    chmod 600 "$CONF"
    echo "conf written: $CONF"
}

do_stop() {
    if [ -f "$DATA/server.pid" ]; then
        kill -9 "$(cat "$DATA/server.pid")" 2>/dev/null || true
        rm -f "$DATA/server.pid"
    fi
    fuser -k -9 "$PORT/tcp" >/dev/null 2>&1 || true
    for _ in $(seq 1 30); do
        ss -ltn 2>/dev/null | grep -q ":$PORT " || return 0
        sleep 0.1
    done
    echo "port $PORT still busy" >&2
    return 1
}

do_start() {
    mkdir -p "$DATA"
    do_stop
    : > "$DATA/server.log"
    (
        cd "$ROOT"
        ulimit -n 65535
        nohup "$BIN/cloud-media-server" -c "$CONF" >>"$DATA/stdout.log" 2>&1 &
        echo $! > "$DATA/server.pid"
    )
    for _ in $(seq 1 50); do
        if grep -q 'listening on' "$DATA/stdout.log" 2>/dev/null; then
            echo "server up: pid=$(cat "$DATA/server.pid") port=$PORT"
            return 0
        fi
        sleep 0.2
    done
    tail -20 "$DATA/server.log" >&2
    return 1
}

do_wipe() {
    sql "SET FOREIGN_KEY_CHECKS=0;
         TRUNCATE TABLE t_user_file;
         TRUNCATE TABLE t_file;
         TRUNCATE TABLE t_chunk;
         TRUNCATE TABLE t_transfer_task;
         TRUNCATE TABLE t_share;
         TRUNCATE TABLE t_play_record;
         SET FOREIGN_KEY_CHECKS=1;" >/dev/null
    rm -rf "$DATA/blobs" "$DATA/tmp"
    mkdir -p "$DATA/blobs" "$DATA/tmp"
    redis-cli -n 1 flushdb >/dev/null 2>&1 || true
    echo "wiped: db=$DB data=$DATA"
}

case "${1:-status}" in
    init)   do_init ;;
    start)  do_start ;;
    stop)   do_stop ;;
    wipe)   do_wipe ;;
    status)
        ss -ltn 2>/dev/null | grep ":$PORT " || echo "port $PORT not listening"
        [ -f "$DATA/server.pid" ] && echo "pid=$(cat "$DATA/server.pid")"
        ;;
    *)
        echo "usage: $0 init|start|stop|wipe|status" >&2
        exit 1
        ;;
esac
