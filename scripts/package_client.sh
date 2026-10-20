#!/bin/bash
# 客户端一键出包：windeployqt + 播放器 DLL → deploy/，并可选做"干净 PATH"自检。
# 用法：
#   scripts/package_client.sh            # 只出包
#   scripts/package_client.sh --verify   # 出包 + 纯净 PATH 启动截图
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-client}"
DEPLOY="${DEPLOY_DIR:-$ROOT/deploy}"
EXE="$BUILD_DIR/bin/cloud-media-client.exe"
QT_BIN="${QT_BIN:-/d/QT/5.12.11/mingw73_32/bin}"
MINGW_BIN="${MINGW_BIN:-/d/QT/Tools/mingw730_32/bin}"
WINDEPLOYQT="$QT_BIN/windeployqt.exe"

# windeployqt 会按 PATH 找编译器运行时；PATH 里若有 64 位 MinGW，会错拷
# libgcc_s_seh-1.dll（64 位），32 位 exe 直接起不来。先把 32 位工具链顶到最前。
export PATH="$MINGW_BIN:$QT_BIN:$PATH"
DLL_DIR="$ROOT/client/player/deps/dll"

[ -f "$EXE" ] || { echo "找不到 $EXE，先构建 build-client"; exit 1; }
[ -x "$WINDEPLOYQT" ] || { echo "找不到 windeployqt：$WINDEPLOYQT"; exit 1; }

echo "1/4 清理 $DEPLOY"
rm -rf "$DEPLOY"
mkdir -p "$DEPLOY"
: > "$DEPLOY/.gitkeep"   # 仓库里的占位文件，重建为空文件，保持工作区干净

echo "2/4 复制 exe"
cp -f "$EXE" "$DEPLOY/"

echo "3/4 windeployqt（Qt DLL + platforms/sqldrivers 插件）"
"$WINDEPLOYQT" --release --no-translations "$DEPLOY/cloud-media-client.exe" >/dev/null

echo "4/4 播放器 DLL（FFmpeg / SDL2）+ 固定 32 位 mingw 运行时"
rm -f "$DEPLOY"/libgcc_s_seh-1.dll
cp -f "$MINGW_BIN/libgcc_s_dw2-1.dll" "$MINGW_BIN/libstdc++-6.dll" "$MINGW_BIN/libwinpthread-1.dll" "$DEPLOY/" 2>/dev/null || true
if [ -d "$DLL_DIR" ]; then
    cp -f "$DLL_DIR"/*.dll "$DEPLOY/"
else
    echo "WARN 缺少 $DLL_DIR，先跑 scripts/sync_player_dlls.sh"
fi

required="Qt5Core.dll Qt5Gui.dll Qt5Widgets.dll Qt5Network.dll Qt5Sql.dll \
platforms/qwindows.dll sqldrivers/qsqlite.dll \
avformat-58.dll avcodec-58.dll avutil-56.dll swscale-5.dll swresample-3.dll SDL2.dll"
missing=0
for f in $required; do
    if [ ! -f "$DEPLOY/$f" ]; then
        echo "MISSING $f"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || { echo "PACKAGE_FAIL 缺少依赖"; exit 1; }

echo "deploy 文件数：$(find "$DEPLOY" -type f | wc -l)，大小：$(du -sh "$DEPLOY" | cut -f1)"

if [ "${1:-}" = "--verify" ]; then
    echo "干净 PATH 自检（只留 /c/Windows/system32:/c/Windows）"
    (
        PATH="/c/Windows/system32:/c/Windows"
        cd "$DEPLOY"
        ./cloud-media-client.exe --version
        ./cloud-media-client.exe --host 192.168.142.131 --port 8004 \
            --tel 13900000011 --password p81-smoke-2026 --shot-delay 4000 \
            --screenshot "$ROOT/docs/screenshots/p8-5-deploy-clean-path.png"
    )
    echo "截图：docs/screenshots/p8-5-deploy-clean-path.png"
fi

echo "PACKAGE_OK $DEPLOY"
