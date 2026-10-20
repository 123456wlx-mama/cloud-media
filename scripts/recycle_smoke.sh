#!/usr/bin/env bash
# 回收站冒烟：delete -> list(可见) -> restore -> list(消失) -> delete -> purge。
set -euo pipefail
: "${CM_CLI_PASSWORD:?set CM_CLI_PASSWORD}"

TEL="${TEL:-13800000001}"
HOST="${E2E_HOST:-127.0.0.1}"
PORT="${E2E_PORT:-8004}"
ROOT="${E2E_ROOT:-/home/colin/cloud-media}"
BIN="$ROOT/build/bin"
CONF="$ROOT/build/e2e.conf"
DATA="${E2E_DATA:-/home/colin/cm_e2e_data}"
TESTDATA="${E2E_TESTDATA:-/home/colin/cm_testdata}"
SRC="${E2E_SRC:-$TESTDATA/test_50m.bin}"
LOG="$DATA/server.log"
OUTDIR="$DATA/recycle_$(date +%s)"

mkdir -p "$OUTDIR"
RC=0
say() { printf '\n=== %s ===\n' "$1"; }
fail() { echo "ASSERT FAILED: $*" >&2; exit 1; }
ok() { echo "PASS: $*"; }
kv() { awk -F= -v k="$2" '$1 == k { print $2; exit }' "$1"; }

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
    ( cd "$ROOT" && nohup "$BIN/cloud-media-server" -c "$CONF" >>"$LOG" 2>&1 & )
    for _ in $(seq 1 50); do
        grep -q 'listening on' "$LOG" && return 0
        sleep 0.2
    done
    cat "$LOG" >&2
    fail "server did not start"
}

CLI_COMMON=(--host "$HOST" --port "$PORT" --tel "$TEL" --password "$CM_CLI_PASSWORD")
RCLI=("$BIN/recycle_cli")

stop_server
start_server
grep 'listening on' "$LOG"

[ -s "$SRC" ] || head -c 52428800 /dev/urandom > "$SRC"

say "upload fixture"
run_capture "$OUTDIR/00_upload.out" "$BIN/upload_cli" "${CLI_COMMON[@]}" \
    --file "$SRC" --remote "recycle_$(date +%s).bin"
[ "$RC" -eq 0 ] || fail "fixture upload"
UF_ID=$(kv "$OUTDIR/00_upload.out" UF_ID)
[ -n "$UF_ID" ] || fail "missing UF_ID"
echo "UF_ID=$UF_ID"

say "delete"
run_capture "$OUTDIR/01_delete.out" "${RCLI[@]}" delete "${CLI_COMMON[@]}" --uf-id "$UF_ID"
[ "$RC" -eq 0 ] || fail "delete exit"
[ "$(kv "$OUTDIR/01_delete.out" CODE)" = "0" ] || fail "delete code"
ok "node soft-deleted"

say "recycle list contains node"
run_capture "$OUTDIR/02_list.out" "${RCLI[@]}" list "${CLI_COMMON[@]}" --limit 100
[ "$RC" -eq 0 ] || fail "list exit"
grep -q "^UF_ID=$UF_ID" "$OUTDIR/02_list.out" || fail "deleted node not in recycle list"
ok "node visible in recycle list"

say "restore"
run_capture "$OUTDIR/03_restore.out" "${RCLI[@]}" restore "${CLI_COMMON[@]}" --uf-id "$UF_ID"
[ "$RC" -eq 0 ] || fail "restore exit"
[ "$(kv "$OUTDIR/03_restore.out" CODE)" = "0" ] || fail "restore code"
[ "$(kv "$OUTDIR/03_restore.out" UF_ID)" = "$UF_ID" ] || fail "restore uid mismatch"
ok "node restored"

say "recycle list no longer contains node"
run_capture "$OUTDIR/04_list_after.out" "${RCLI[@]}" list "${CLI_COMMON[@]}" --limit 100
[ "$RC" -eq 0 ] || fail "list exit"
if grep -q "^UF_ID=$UF_ID" "$OUTDIR/04_list_after.out"; then
    fail "restored node still in recycle list"
fi
ok "node absent after restore"

say "delete + purge"
run_capture "$OUTDIR/05_delete2.out" "${RCLI[@]}" delete "${CLI_COMMON[@]}" --uf-id "$UF_ID"
[ "$RC" -eq 0 ] || fail "second delete exit"
run_capture "$OUTDIR/06_purge.out" "${RCLI[@]}" purge "${CLI_COMMON[@]}" --uf-id "$UF_ID"
[ "$RC" -eq 0 ] || fail "purge exit"
[ "$(kv "$OUTDIR/06_purge.out" FILES_DELETED)" = "1" ] || fail "purge filesDeleted"
ok "node purged"

stop_server
say "ALL PASSED"
echo "OUTDIR=$OUTDIR"
