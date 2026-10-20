#!/bin/bash
# P8 全链路 demo（可重复执行）：登录 → 上传 mp4 → 分享出码 → 新进程打开分享 → 下载 → 边下边播到结尾。
# 一条连续日志（build-client/e2e-run/e2e_p8.log），结尾打印 FULL_LINK_OK。
#
# 用法：scripts/e2e_p8.sh
# 可覆盖：CM_HOST / CM_PORT / CM_TEL / CM_PASSWORD / CM_VIDEO / RUN_DIR / CLIENT
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLIENT="${CLIENT:-$ROOT/build-client/bin/cloud-media-client.exe}"
HOST="${CM_HOST:-192.168.142.131}"
PORT="${CM_PORT:-8004}"
TEL="${CM_TEL:-13900000011}"
PASS="${CM_PASSWORD:-p81-smoke-2026}"
VIDEO="${CM_VIDEO:-$ROOT/build-client/run/p85_demo.mp4}"
RUN_DIR="${RUN_DIR:-$ROOT/build-client/e2e-run}"
LOG="$RUN_DIR/e2e_p8.log"
SHOT_DIR="$ROOT/docs/screenshots"

export PATH="$ROOT/client/player/deps/dll:/d/QT/Tools/mingw730_32/bin:/d/QT/5.12.11/mingw73_32/bin:$PATH"
mkdir -p "$RUN_DIR" "$SHOT_DIR" || exit 1
cd "$RUN_DIR" || exit 1
: > "$LOG"

say() { printf '%s\n' "$*" | tee -a "$LOG"; }
step() { say ""; say "=== $* ==="; }
fail() { say "E2E_FAIL $*"; exit 1; }

call() {
    "$CLIENT" --host "$HOST" --port "$PORT" --tel "$TEL" --password "$PASS" "$@" 2>&1 | tee -a "$LOG"
    return ${PIPESTATUS[0]}
}

[ -f "$VIDEO" ] || fail "找不到视频：$VIDEO"

# Windows 临时目录里的旧 .part（QDir::tempPath()）
TMP_WIN="${TEMP:-}"
if [ -n "$TMP_WIN" ] && command -v cygpath >/dev/null 2>&1; then
    TMP_UNIX="$(cygpath -u "$TMP_WIN")"
else
    TMP_UNIX="${TMPDIR:-/tmp}"
fi
rm -f "$TMP_UNIX"/p85_demo.mp4.part* 2>/dev/null

FFMPEG="${CM_FFMPEG:-/d/mediaplayer/MediaPlayer/ffmpeg-4.2.2/bin/ffmpeg.exe}"
STAMP="$(date +%s)"
NAME="e2e_p85_$STAMP.mp4"
if [ -x "$FFMPEG" ] || [ -f "$FFMPEG" ]; then
    # 每轮换一个 comment，md5 就不同 → 走真实分片上傳（否则第二次起全是秒传）
    "$FFMPEG" -y -v error -i "$VIDEO" -c copy -metadata comment="e2e-$STAMP" \
        -movflags +faststart "$RUN_DIR/$NAME" >/dev/null 2>&1 \
        || cp -f "$VIDEO" "$RUN_DIR/$NAME"
    VIDEO="$RUN_DIR/$NAME"
fi
NAME="$(basename "$VIDEO")"
rm -f "$RUN_DIR/cm-tasks.db"   # 每次从干净任务库开始（否则上一轮任务会被恢复成暂停）

say "e2e target: $HOST:$PORT tel=$TEL video=$NAME"

step "1/5 登录 + 上传（UploadInit/Chunk/Complete）"
OUT="$(call --smoke --upload "$VIDEO")" || fail "上传进程退出码非 0"
printf '%s\n' "$OUT" | grep -q "^LOGIN_OK" || fail "登录失败"
printf '%s\n' "$OUT" | grep -qE "state=(已完成|已秒传)" || fail "上传未完成"

step "2/5 列表定位 ufId"
OUT="$(call --smoke)" || fail "列表进程退出码非 0"
UF="$(printf '%s\n' "$OUT" | awk -v n="$NAME" '$1=="ROW" && $3==n { split($2,a,"="); print a[2]; exit }')"
[ -n "$UF" ] || fail "列表里找不到 $NAME"
say "UF_ID=$UF"

step "3/5 右键分享（ShareCreateRq）"
OUT="$(call --smoke --share-create "$UF")" || fail "分享进程退出码非 0"
CODE="$(printf '%s\n' "$OUT" | sed -n 's/^SHARE code=\([A-Z0-9]*\).*/\1/p' | head -1)"
ACCESS="$(printf '%s\n' "$OUT" | sed -n 's/^SHARE .* access=\([0-9]*\).*/\1/p' | head -1)"
[ -n "$CODE" ] || fail "没拿到分享码"
say "SHARE_CODE=$CODE ACCESS=${ACCESS:-none}"

step "4/5 新进程打开分享（ShareOpenRq）+ 下载 + 边下边播到结尾"
OUT="$(call --play-share "$CODE" --access "$ACCESS")" || fail "播放进程退出码非 0"
printf '%s\n' "$OUT" | grep -q "SHARE_OPEN_OK" || fail "打开分享失败"
printf '%s\n' "$OUT" | grep -q "PLAY_STREAM_SOURCE_DONE" || fail "下载未完成"
printf '%s\n' "$OUT" | grep -q "PLAY_STREAM_ENDED" || fail "没有播到结尾"

step "5/5 关键节点截图（分享弹窗 / 播放中）"
"$CLIENT" --host "$HOST" --port "$PORT" --tel "$TEL" --password "$PASS" \
    --ui-share "$UF" --shot-delay 4000 \
    --screenshot "$SHOT_DIR/p8-5-share-dialog.png" >/dev/null 2>&1 || say "WARN 分享弹窗截图失败"
"$CLIENT" --host "$HOST" --port "$PORT" --tel "$TEL" --password "$PASS" \
    --play-remote "$UF" --shot-delay 6000 \
    --screenshot "$SHOT_DIR/p8-5-playing.png" >/dev/null 2>&1 || say "WARN 播放截图失败"
[ -f "$SHOT_DIR/p8-5-share-dialog.png" ] || say "WARN 缺 p8-5-share-dialog.png"
[ -f "$SHOT_DIR/p8-5-playing.png" ] || say "WARN 缺 p8-5-playing.png"

say ""
say "UF_ID=$UF SHARE_CODE=$CODE ACCESS=${ACCESS:-none}"
say "LOG=$LOG"
say "FULL_LINK_OK"
exit 0
