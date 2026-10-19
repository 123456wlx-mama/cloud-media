#!/usr/bin/env bash
# Phase 5 E-4 缓存降级演练。
# 密码只从环境读取：CM_CLI_PASSWORD / CM_MYSQL_PASSWORD / CM_SUDO_PASSWORD。
# 每一步失败立即 exit 1；不修改任何鉴权实现，只记录实际行为与 06 §10 的差异。
set -euo pipefail

: "${CM_CLI_PASSWORD:?set CM_CLI_PASSWORD}"
: "${CM_MYSQL_PASSWORD:?set CM_MYSQL_PASSWORD}"
: "${CM_SUDO_PASSWORD:?set CM_SUDO_PASSWORD}"

TEL="${TEL:-13800000001}"
MYSQL_USER="${CM_MYSQL_USER:-cm_test}"
MYSQL_DB="${CM_MYSQL_DB:-cloud_media_test}"
HOST="${E2E_HOST:-127.0.0.1}"
PORT="${E2E_PORT:-8004}"
ROOT="${E2E_ROOT:-/home/colin/cloud-media}"
BIN="$ROOT/build/bin"
CONF="$ROOT/build/e2e.conf"
DATA="${E2E_DATA:-/home/colin/cm_e2e_data}"
TESTDATA="${E2E_TESTDATA:-/home/colin/cm_testdata}"
SRC="${E2E_SRC:-$TESTDATA/test_50m.bin}"
LOG="$DATA/server.log"
RUN_ID="$(date +%s)"
SHARE_NOCODE="E2EABC"
SHARE_CODE="E2EDEF"
ACCESS_CODE="2468"
OUTDIR="$DATA/degrade_$RUN_ID"

mkdir -p "$OUTDIR" "$DATA" "$TESTDATA"
chmod 700 "$DATA" 2>/dev/null || true

RC=0
say() { printf '\n=== %s ===\n' "$1"; }
fail() { echo "ASSERT FAILED: $*" >&2; exit 1; }
ok() { echo "PASS: $*"; }

sql() { MYSQL_PWD="$CM_MYSQL_PASSWORD" mysql -N -B -u"$MYSQL_USER" "$MYSQL_DB" -e "$1"; }
sudo_run() { echo "$CM_SUDO_PASSWORD" | sudo -S -p '' bash -c "$*"; }
kv() { awk -F= -v k="$2" '$1 == k { print $2; exit }' "$1"; }
code_of() { grep -oE 'CODE=[0-9]+|code=[0-9]+' "$1" | head -1 | cut -d= -f2; }

run_capture() {
    local out="$1"
    shift
    set +e
    "$@" >"$out" 2>&1
    RC=$?
    set -e
    echo "--- $(basename "$1") -> exit $RC"
    cat "$out"
}

stop_server() {
    local pids
    pids=$(ps -eo pid,args | awk '/[c]loud-media-server -c/ {print $1}')
    for p in $pids; do kill -9 "$p" 2>/dev/null || true; done
    sleep 1
}

start_server() {
    : > "$LOG"
    ( cd "$ROOT" && nohup "$BIN/cloud-media-server" -c "$CONF" >>"$LOG" 2>&1 & )
    for _ in $(seq 1 50); do
        grep -q 'listening on' "$LOG" && return 0
        sleep 0.2
    done
    cat "$LOG" >&2
    fail "server did not start"
}

wait_redis_down() {
    for _ in $(seq 1 50); do
        redis-cli -h 127.0.0.1 -p 6379 ping >/dev/null 2>&1 || return 0
        sleep 0.1
    done
    return 1
}

wait_redis_up() {
    for _ in $(seq 1 50); do
        redis-cli -h 127.0.0.1 -p 6379 ping 2>/dev/null | grep -q PONG && return 0
        sleep 0.1
    done
    return 1
}

wait_mysql_down() {
    for _ in $(seq 1 80); do
        if ! MYSQL_PWD="$CM_MYSQL_PASSWORD" mysqladmin -u"$MYSQL_USER" ping >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.25
    done
    return 1
}

assert_code_in() {
    local file="$1" label="$2"
    shift 2
    local c
    c=$(code_of "$file")
    [ -n "$c" ] || fail "$label: no CODE/code in output"
    for allowed in "$@"; do
        if [ "$c" = "$allowed" ]; then
            echo "$label CODE=$c"
            return 0
        fi
    done
    fail "$label: CODE=$c not in {$*}"
}

# ---------------------------------------------------------------- 准备
stop_server
# 演练前保证 MySQL/Redis 都在
MYSQL_PWD="$CM_MYSQL_PASSWORD" mysqladmin -u"$MYSQL_USER" ping >/dev/null 2>&1 || sudo_run "service mysql start"
redis-cli ping >/dev/null 2>&1 || sudo_run "redis-server /etc/redis/redis.conf --daemonize yes"
sleep 1
start_server
grep 'listening on' "$LOG"

[ -s "$SRC" ] || head -c 52428800 /dev/urandom > "$SRC"
MD5_REF=$(md5sum "$SRC" | cut -d' ' -f1)

run_capture "$OUTDIR/00_upload.out" "$BIN/upload_cli" \
    --host "$HOST" --port "$PORT" --tel "$TEL" --password "$CM_CLI_PASSWORD" \
    --file "$SRC" --remote "degrade_$RUN_ID.bin"
[ "$RC" -eq 0 ] || fail "initial upload"
UF_ID=$(kv "$OUTDIR/00_upload.out" UF_ID)
[ -n "$UF_ID" ] || fail "missing UF_ID"
UID_VAL=$(sql "SELECT u_id FROM t_user WHERE u_tel='$TEL'")
FID=$(sql "SELECT f_id FROM t_user_file WHERE uf_id=$UF_ID")
[ -n "$UID_VAL" ] && [ -n "$FID" ] || fail "cannot resolve uid/fid"
echo "UF_ID=$UF_ID FID=$FID UID=$UID_VAL MD5_REF=$MD5_REF"

sql "INSERT INTO t_share (s_code,s_user_id,s_uf_id,s_file_id,s_access_code,s_status,s_expire_at)
     VALUES ('$SHARE_NOCODE',$UID_VAL,$UF_ID,$FID,NULL,0,DATE_ADD(NOW(6), INTERVAL 1 DAY))
     ON DUPLICATE KEY UPDATE s_user_id=VALUES(s_user_id), s_uf_id=VALUES(s_uf_id),
       s_file_id=VALUES(s_file_id), s_access_code=NULL, s_status=0,
       s_expire_at=VALUES(s_expire_at)" >/dev/null
sql "INSERT INTO t_share (s_code,s_user_id,s_uf_id,s_file_id,s_access_code,s_status,s_expire_at)
     VALUES ('$SHARE_CODE',$UID_VAL,$UF_ID,$FID,'$ACCESS_CODE',0,DATE_ADD(NOW(6), INTERVAL 1 DAY))
     ON DUPLICATE KEY UPDATE s_user_id=VALUES(s_user_id), s_uf_id=VALUES(s_uf_id),
       s_file_id=VALUES(s_file_id), s_access_code=VALUES(s_access_code), s_status=0,
       s_expire_at=VALUES(s_expire_at)" >/dev/null
echo "shares ready: $SHARE_NOCODE / $SHARE_CODE"

# ---------------------------------------------------------------- Redis 故障
say "Redis DOWN"
redis-cli shutdown nosave >/dev/null 2>&1 || true
wait_redis_down || fail "redis did not stop"
echo "redis is down"

run_capture "$OUTDIR/10_login_list.out" "$BIN/list_cli" \
    --host "$HOST" --port "$PORT" --tel "$TEL" --password "$CM_CLI_PASSWORD" --limit 1
[ "$RC" -eq 0 ] || fail "login should succeed when Redis is down"
ok "login + list work with Redis down"

run_capture "$OUTDIR/11_instant.out" "$BIN/upload_cli" \
    --host "$HOST" --port "$PORT" --tel "$TEL" --password "$CM_CLI_PASSWORD" \
    --file "$SRC" --remote "degrade_$RUN_ID.bin"
[ "$RC" -eq 0 ] || fail "instant upload should succeed when Redis is down"
grep -q 'STATUS=INSTANT_HIT' "$OUTDIR/11_instant.out" || fail "expected instant hit"
ok "instant upload falls back to MySQL"

run_capture "$OUTDIR/12_download_uf.out" "$BIN/download_cli" \
    --host "$HOST" --port "$PORT" --tel "$TEL" --password "$CM_CLI_PASSWORD" \
    --uf-id "$UF_ID" --out "$OUTDIR/dl_uf.bin"
[ "$RC" -eq 4 ] || fail "download open must fail closed (exit 4), got $RC"
assert_code_in "$OUTDIR/12_download_uf.out" "download-open" 4005 4007
ok "download open fail-closed"

run_capture "$OUTDIR/13_share_access.out" "$BIN/download_cli" \
    --host "$HOST" --port "$PORT" --tel "$TEL" --password "$CM_CLI_PASSWORD" \
    --share-code "$SHARE_CODE" --access-code "$ACCESS_CODE" --out "$OUTDIR/dl_share_access.bin"
[ "$RC" -eq 4 ] || fail "share with access code must fail closed (exit 4), got $RC"
# 实际：10022 ShareOpen 未在 main 注册，handler_registry 回 9003 UnknownType。
# 安全属性（拒绝）成立，但与 06 §10 的 4008 不一致，记为 GAP。
assert_code_in "$OUTDIR/13_share_access.out" "share-access" 4008
ok "share access-code fail-closed"

run_capture "$OUTDIR/14_share_nocode.out" "$BIN/download_cli" \
    --host "$HOST" --port "$PORT" --tel "$TEL" --password "$CM_CLI_PASSWORD" \
    --share-code "$SHARE_NOCODE" --out "$OUTDIR/dl_share_nocode.bin"
[ "$RC" -eq 4 ] || fail "share without access code must not download (exit 4), got $RC"
# 10022 元信息回源成功（MySQL），但 Redis 挂时 share_ticket 为空；
# 10010 收到 ufId=0 + 空 ticket 必须 1001 拒绝，绝不按 ufId 回退。
assert_code_in "$OUTDIR/14_share_nocode.out" "share-nocode" 1001
ok "share without access code fails closed (empty ticket)"

run_capture "$OUTDIR/15_list.out" "$BIN/list_cli" \
    --host "$HOST" --port "$PORT" --tel "$TEL" --password "$CM_CLI_PASSWORD" --limit 5
[ "$RC" -eq 0 ] || fail "list should succeed when Redis is down"
grep -q '^HAS_MORE=' "$OUTDIR/15_list.out" || fail "list output missing HAS_MORE"
ok "list works with Redis down"

# ---------------------------------------------------------------- MySQL 故障
say "Redis UP again"
sudo_run "redis-server /etc/redis/redis.conf --daemonize yes" >/dev/null 2>&1 || true
wait_redis_up || fail "redis did not restart before MySQL phase"
echo "redis is up"

say "MySQL DOWN"
sudo_run "service mysql stop" >/dev/null
wait_mysql_down || fail "mysql did not stop"
echo "mysql is down"

run_capture "$OUTDIR/20_login.out" "$BIN/list_cli" \
    --host "$HOST" --port "$PORT" --tel "$TEL" --password "$CM_CLI_PASSWORD" --limit 1
[ "$RC" -ne 0 ] || fail "login must fail when MySQL is down"
assert_code_in "$OUTDIR/20_login.out" "login" 4001 4002
ok "login fails closed"

run_capture "$OUTDIR/21_upload.out" "$BIN/upload_cli" \
    --host "$HOST" --port "$PORT" --tel "$TEL" --password "$CM_CLI_PASSWORD" \
    --file "$SRC" --remote "degrade_down_$RUN_ID.bin"
[ "$RC" -ne 0 ] || fail "upload must fail when MySQL is down"
assert_code_in "$OUTDIR/21_upload.out" "upload" 4001 4002
ok "upload fails closed"

run_capture "$OUTDIR/22_download.out" "$BIN/download_cli" \
    --host "$HOST" --port "$PORT" --tel "$TEL" --password "$CM_CLI_PASSWORD" \
    --uf-id "$UF_ID" --out "$OUTDIR/dl_down.bin"
[ "$RC" -ne 0 ] || fail "download must fail when MySQL is down"
assert_code_in "$OUTDIR/22_download.out" "download" 4001 4002
ok "download fails closed"

run_capture "$OUTDIR/23_list.out" "$BIN/list_cli" \
    --host "$HOST" --port "$PORT" --tel "$TEL" --password "$CM_CLI_PASSWORD" --limit 5
[ "$RC" -ne 0 ] || fail "list must not succeed when MySQL is down"
if grep -q '^HAS_MORE=' "$OUTDIR/23_list.out"; then
    fail "list returned an empty/success payload while MySQL is down"
fi
assert_code_in "$OUTDIR/23_list.out" "list" 4001 4002
ok "list never fakes empty success"

# ---------------------------------------------------------------- 恢复自愈
say "MySQL RECOVERY (<=5s)"
sudo_run "service mysql start" >/dev/null
T0=$(date +%s%3N)
HEALED=0
for _ in $(seq 1 50); do
    set +e
    "$BIN/list_cli" --host "$HOST" --port "$PORT" --tel "$TEL" \
        --password "$CM_CLI_PASSWORD" --limit 1 >"$OUTDIR/30_recover.out" 2>&1
    RC=$?
    set -e
    if [ "$RC" -eq 0 ]; then
        HEALED=1
        break
    fi
    sleep 0.1
done
T1=$(date +%s%3N)
HEAL_MS=$((T1 - T0))
cat "$OUTDIR/30_recover.out"
[ "$HEALED" -eq 1 ] || fail "pool did not recover within 5s"
[ "$HEAL_MS" -le 5000 ] || fail "recovery took ${HEAL_MS}ms (>5000ms)"
echo "RECOVERY_MS=$HEAL_MS"
ok "MySQL pool self-heals"

# Redis/MySQL 都恢复后，分享链路应能真正下载（验证 shareTicket 支持）
say "SHARE DOWNLOAD (Redis+MySQL up)"
SHARE_FILE="$OUTDIR/dl_share_ok.bin"
run_capture "$OUTDIR/31_share_ok.out" "$BIN/download_cli" \
    --host "$HOST" --port "$PORT" --tel "$TEL" --password "$CM_CLI_PASSWORD" \
    --share-code "$SHARE_NOCODE" --out "$SHARE_FILE"
[ "$RC" -eq 0 ] || fail "share download should succeed after recovery"
MD5_SHARE=$(md5sum "$SHARE_FILE" | cut -d' ' -f1)
[ "$MD5_SHARE" = "$MD5_REF" ] || fail "share download MD5 mismatch"
echo "SHARE_MD5=$MD5_SHARE"
ok "share ticket download works"

# 恢复 Redis（保持环境可用）
wait_redis_up || sudo_run "redis-server /etc/redis/redis.conf --daemonize yes"
wait_redis_up || fail "redis did not restart"

# 清理分享
sql "DELETE FROM t_share WHERE s_code IN ('$SHARE_NOCODE','$SHARE_CODE')" >/dev/null || true
stop_server

say "ALL PASSED"
echo "OUTDIR=$OUTDIR"
echo "RECOVERY_MS=$HEAL_MS"
