#!/bin/bash
# P9-1 播放器回归：seek 风暴 / 暂停恢复 / 音量 0-100 / 纯视频无音频。
# 每条都用 --play-selftest 跑一遍，日志留在 build-client/regress/，结尾打印 PLAYER_REGRESS_OK。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLIENT="${CLIENT:-$ROOT/build-client/bin/cloud-media-client.exe}"
VIDEO="${CM_VIDEO:-D:/mediaplayer/test.mp4}"
FFMPEG="${CM_FFMPEG:-/d/mediaplayer/MediaPlayer/ffmpeg-4.2.2/bin/ffmpeg.exe}"
RUN="${RUN_DIR:-$ROOT/build-client/regress}"

export PATH="$ROOT/client/player/deps/dll:/d/QT/Tools/mingw730_32/bin:/d/QT/5.12.11/mingw73_32/bin:$PATH"
mkdir -p "$RUN" || exit 1
cd "$RUN" || exit 1

[ -f "$VIDEO" ] || { echo "找不到视频：$VIDEO"; exit 1; }
[ -f "$CLIENT" ] || { echo "找不到客户端：$CLIENT"; exit 1; }

# 纯视频（无音频轨）素材：从带音频的视频里剥掉音轨，仍用 faststart
if [ ! -f "$RUN/video_only.mp4" ] && [ -f "$FFMPEG" ]; then
    "$FFMPEG" -y -v error -i "$VIDEO" -an -c copy -movflags +faststart "$RUN/video_only.mp4"
fi
[ -f "$RUN/video_only.mp4" ] || { echo "缺少 video_only.mp4，且 ffmpeg 不可用"; exit 1; }

echo "1/2 带音频：seek 风暴 / 暂停恢复 / 音量"
"$CLIENT" --play-selftest "$VIDEO" > "$RUN/av_sync.log" 2>&1
AV_RC=$?

echo "2/2 纯视频（无音频轨）"
"$CLIENT" --play-selftest "$RUN/video_only.mp4" > "$RUN/video_only.log" 2>&1
VO_RC=$?

fail() { echo "REGRESS_FAIL $*"; exit 1; }
[ "$AV_RC" -eq 0 ] || fail "带音频自检退出码 $AV_RC"
[ "$VO_RC" -eq 0 ] || fail "无音频自检退出码 $VO_RC"
grep -q "SELFTEST_OK" "$RUN/av_sync.log" || fail "av_sync.log 没有 SELFTEST_OK"
grep -q "SELFTEST_OK" "$RUN/video_only.log" || fail "video_only.log 没有 SELFTEST_OK"
grep -q "SELFTEST_OPEN .*has_audio=1" "$RUN/av_sync.log" || fail "带音频文件没识别出音轨"
grep -q "SELFTEST_OPEN .*has_audio=0" "$RUN/video_only.log" || fail "纯视频文件不该有音轨"

# 阈值：seek 风暴后最大漂移 <=200ms（丢帧阈值 120ms + 关键帧粒度），暂停恢复 <=100ms
storm=$(sed -n 's/.*SELFTEST\[seek-storm\].*max_drift_ms=\([0-9]*\).*/\1/p' "$RUN/av_sync.log" | head -1)
pr=$(sed -n 's/.*SELFTEST\[pause-resume\].*max_drift_ms=\([0-9]*\).*/\1/p' "$RUN/av_sync.log" | head -1)
storm_vo=$(sed -n 's/.*SELFTEST\[seek-storm\].*max_drift_ms=\([0-9]*\).*/\1/p' "$RUN/video_only.log" | head -1)
[ -n "$storm" ] && [ -n "$pr" ] && [ -n "$storm_vo" ] || fail "日志里没有漂移数据"

awk -v a="$storm" 'BEGIN{exit !(a<=200)}' || fail "seek 风暴漂移 ${storm}ms > 200ms"
awk -v a="$pr" 'BEGIN{exit !(a<=100)}' || fail "暂停恢复漂移 ${pr}ms > 100ms"
awk -v a="$storm_vo" 'BEGIN{exit !(a<=100)}' || fail "无音频 seek 风暴漂移 ${storm_vo}ms > 100ms"

echo ""
echo "av_sync:    seek_storm=${storm}ms pause_resume=${pr}ms"
echo "video_only: seek_storm=${storm_vo}ms (has_audio=0)"
echo "日志：$RUN/av_sync.log, $RUN/video_only.log"
echo "PLAYER_REGRESS_OK"
