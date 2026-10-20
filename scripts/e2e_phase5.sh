#!/usr/bin/env bash
# Phase 5 E2E：上传断点续传 / 上传中杀服务端 / 下载断点续传。
# 密码只从环境变量读取，脚本不落任何明文口令。
#
# 必填环境：
#   CM_CLI_PASSWORD     测试账号密码
#   CM_MYSQL_PASSWORD   测试库密码（cm_test）
# 可选环境：
#   MD5_REF             期望的 50MB 文件 MD5；不设置则按本轮生成文件计算
#   TEL / CM_MYSQL_USER / E2E_HOST / E2E_PORT
set -euo pipefail

: "${CM_CLI_PASSWORD:?set CM_CLI_PASSWORD}"
: "${CM_MYSQL_PASSWORD:?set CM_MYSQL_PASSWORD}"

TEL="${TEL:-13800000001}"
MYSQL_USER="${CM_MYSQL_USER:-cm_test}"
MYSQL_DB="${CM_MYSQL_DB:-cloud_media_test}"
HOST="${E2E_HOST:-127.0.0.1}"
PORT="${E2E_PORT:-8004}"
ROOT="${E2E_ROOT:-/home/colin/cloud-media}"
BIN="${E2E_BIN:-$ROOT/build/bin}"
CONF="$ROOT/build/e2e.conf"
DATA="${E2E_DATA:-/home/colin/cm_e2e_data}"
TESTDATA="${E2E_TESTDATA:-/home/colin/cm_testdata}"
RUN_ID="$(date +%s)"
LOG="$DATA/server.log"

mkdir -p "$DATA" "$TESTDATA"
chmod 700 "$DATA" 2>/dev/null || true

say() { printf '\n=== %s ===\n' "$1"; }

fail() { echo "ASSERT FAILED: $*" >&2; exit 1; }

assert_eq() {
    [ "$1" = "$2" ] || fail "$3 (got '$1', want '$2')"
}

sql() {
    MYSQL_PWD="$CM_MYSQL_PASSWORD" mysql -N -B -u"$MYSQL_USER" "$MYSQL_DB" -e "$1"
}

kv() {
    awk -F= -v k="$2" '$1 == k { print $2; exit }' "$1"
}

stop_server() {
    local pids
    pids=$(ps -eo pid,args | awk '/[c]loud-media-server -c/ {print $1}')
    for p in $pids; do kill -9 "$p" 2>/dev/null || true; done
    rm -f "$DATA/server.pid"
    # 端口兜底：ps 列表可能漏掉刚 fork 的实例，导致重启时 bind 失败。
    fuser -k -9 8004/tcp >/dev/null 2>&1 || true
    for _ in $(seq 1 50); do
        if ss -ltn 2>/dev/null | grep -q ':8004 '; then
            sleep 0.1
        else
            return 0
        fi
    done
    echo "port 8004 still in use" >&2
    return 1
}

start_server() {
    : > "$LOG"
    ( cd "$ROOT" && nohup "$BIN/cloud-media-server" -c "$CONF" >>"$LOG" 2>&1 & echo $! > "$DATA/server.pid" )
    for _ in $(seq 1 50); do
        if grep -q 'listening on' "$LOG"; then
            return 0
        fi
        sleep 0.2
    done
    cat "$LOG" >&2
    fail "server did not start"
}

# 运行并保留输出，期望退出码 9；同时断言 state 已落盘。
run_expect9() {
    local out="$1"
    shift
    set +e
    "$@" >"$out" 2>&1
    local rc=$?
    set -e
    cat "$out"
    assert_eq "$rc" "9" "expected _exit(9) from $1"
}

run_ok() {
    local out="$1"
    shift
    set +e
    "$@" >"$out" 2>&1
    local rc=$?
    set -e
    cat "$out"
    assert_eq "$rc" "0" "expected exit 0 from $1"
}

make_50m() {
    local path="$1"
    if [ ! -s "$path" ]; then
        head -c 52428800 /dev/urandom > "$path"
    fi
}

stop_server
T_ALL0=$SECONDS
start_server
grep 'listening on' "$LOG"

# ---------------------------------------------------------------- A: 上传续传
say "A) upload interrupt + resume"
T_STEP=$SECONDS
A_SRC="$TESTDATA/e2e_a_${RUN_ID}.bin"
A_STATE="$TESTDATA/e2e_a_${RUN_ID}.state"
A_OUT="$TESTDATA/e2e_a_${RUN_ID}.out"
make_50m "$A_SRC"
MD5_A="$(md5sum "$A_SRC" | cut -d' ' -f1)"
[ -z "${MD5_REF:-}" ] || assert_eq "$MD5_A" "$MD5_REF" "source MD5 differs from MD5_REF"
echo "MD5_A=$MD5_A"

run_expect9 "$A_OUT" "$BIN/upload_cli" --host "$HOST" --port "$PORT" \
    --tel "$TEL" --password "$CM_CLI_PASSWORD" \
    --file "$A_SRC" --remote "e2e_a_${RUN_ID}.bin" \
    --state "$A_STATE" --stop-after-chunks 6

A_TASK="$(kv "$A_OUT" TASK_ID)"
[ -n "$A_TASK" ] || fail "missing TASK_ID"
[ -s "$A_STATE" ] || fail "upload state not persisted"

A_DONE="$(sql "SELECT tr_done_chunks FROM t_transfer_task WHERE tr_task_id='$A_TASK'")"
A_STATUS="$(sql "SELECT tr_status FROM t_transfer_task WHERE tr_task_id='$A_TASK'")"
echo "A_TASK=$A_TASK status=$A_STATUS done_chunks=$A_DONE"
assert_eq "$A_DONE" "6" "A done_chunks"

A_RESUME_OUT="$TESTDATA/e2e_a_${RUN_ID}.resume.out"
run_ok "$A_RESUME_OUT" "$BIN/upload_cli" --host "$HOST" --port "$PORT" \
    --tel "$TEL" --password "$CM_CLI_PASSWORD" \
    --file "$A_SRC" --state "$A_STATE" -r
MD5_A_FINAL="$(kv "$A_RESUME_OUT" FILE_MD5)"
UF_ID="$(kv "$A_RESUME_OUT" UF_ID)"
assert_eq "$MD5_A_FINAL" "$MD5_A" "A final MD5"
[ -n "$UF_ID" ] || fail "missing UF_ID"
echo "A_FINAL_MD5=$MD5_A_FINAL UF_ID=$UF_ID"
echo "A_SECONDS=$((SECONDS - T_STEP))"

# ------------------------------------------------------- B: 上传中杀服务端
say "B) kill server mid-upload, restart, resume"
T_STEP=$SECONDS
B_SRC="$TESTDATA/e2e_b_${RUN_ID}.bin"
B_STATE="$TESTDATA/e2e_b_${RUN_ID}.state"
B_OUT="$TESTDATA/e2e_b_${RUN_ID}.out"
make_50m "$B_SRC"
MD5_B="$(md5sum "$B_SRC" | cut -d' ' -f1)"
echo "MD5_B=$MD5_B"

run_expect9 "$B_OUT" "$BIN/upload_cli" --host "$HOST" --port "$PORT" \
    --tel "$TEL" --password "$CM_CLI_PASSWORD" \
    --file "$B_SRC" --remote "e2e_b_${RUN_ID}.bin" \
    --state "$B_STATE" --stop-after-chunks 6
B_TASK="$(kv "$B_OUT" TASK_ID)"
[ -n "$B_TASK" ] || fail "missing B TASK_ID"

stop_server
B_STATUS_KILLED="$(sql "SELECT tr_status FROM t_transfer_task WHERE tr_task_id='$B_TASK'")"
B_DONE_KILLED="$(sql "SELECT tr_done_chunks FROM t_transfer_task WHERE tr_task_id='$B_TASK'")"
echo "B after kill: status=$B_STATUS_KILLED done_chunks=$B_DONE_KILLED"

# 中途中止不会产生 7；为覆盖 U-6 的「合并中崩溃 7→2」分支，注入 7 后再重启。
sql "UPDATE t_transfer_task SET tr_status=7 WHERE tr_task_id='$B_TASK'" >/dev/null
start_server
B_STATUS_RECOVERED="$(sql "SELECT tr_status FROM t_transfer_task WHERE tr_task_id='$B_TASK'")"
echo "B after restart: status=$B_STATUS_RECOVERED"
assert_eq "$B_STATUS_RECOVERED" "2" "7->2 recovery"

B_RESUME_OUT="$TESTDATA/e2e_b_${RUN_ID}.resume.out"
run_ok "$B_RESUME_OUT" "$BIN/upload_cli" --host "$HOST" --port "$PORT" \
    --tel "$TEL" --password "$CM_CLI_PASSWORD" \
    --file "$B_SRC" --state "$B_STATE" -r
MD5_B_FINAL="$(kv "$B_RESUME_OUT" FILE_MD5)"
assert_eq "$MD5_B_FINAL" "$MD5_B" "B final MD5"
echo "B_FINAL_MD5=$MD5_B_FINAL"
echo "B_SECONDS=$((SECONDS - T_STEP))"

# ------------------------------------------------------------ C: 下载续传
say "C) download interrupt + resume"
T_STEP=$SECONDS
DL_OUT_FILE="$TESTDATA/e2e_dl_${RUN_ID}.bin"
DL_OUT="$TESTDATA/e2e_dl_${RUN_ID}.out"
DL_RESUME="$TESTDATA/e2e_dl_${RUN_ID}.resume.out"
rm -f "$DL_OUT_FILE" "$DL_OUT_FILE.part" "$DL_OUT_FILE.part.state"

run_expect9 "$DL_OUT" "$BIN/download_cli" --host "$HOST" --port "$PORT" \
    --tel "$TEL" --password "$CM_CLI_PASSWORD" \
    --uf-id "$UF_ID" --out "$DL_OUT_FILE" --stop-after-blocks 6
[ -s "$DL_OUT_FILE.part.state" ] || fail "download state not persisted"
echo "C part.state:"; cat "$DL_OUT_FILE.part.state"

run_ok "$DL_RESUME" "$BIN/download_cli" --host "$HOST" --port "$PORT" \
    --tel "$TEL" --password "$CM_CLI_PASSWORD" \
    --uf-id "$UF_ID" --out "$DL_OUT_FILE" -r
MD5_DL="$(md5sum "$DL_OUT_FILE" | cut -d' ' -f1)"
assert_eq "$MD5_DL" "$MD5_A" "C downloaded MD5"
echo "C_MD5=$MD5_DL"
echo "C_SECONDS=$((SECONDS - T_STEP))"

echo "ALL_SECONDS=$((SECONDS - T_ALL0))"
say "ALL PASSED"
echo "SERVER_LOG=$LOG"
echo "MD5_REF=$MD5_A"
