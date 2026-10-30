# ============================================================================
#  cloud-media 客户端（qmake 版）
#
#  用法：Qt Creator 打开本文件 → 选 "Desktop Qt 5.12.11 MinGW 32bit" → 构建/运行
#  等价于 CMake 的 cloud-media-client target；不需要 CMake。
#
#  依赖：Qt5 Widgets/Network/Sql
#        client/player/deps 下的 FFmpeg 4.2.2 + SDL2 2.0.10（32 位，随仓库提供）
# ============================================================================

QT += core gui widgets network sql

CONFIG += c++11 console
CONFIG -= app_bundle
# common/base/version.cpp 与 common/protocol/version.cpp 同名：
# 不开这个选项 qmake 会把两者都编成 version.o 互相覆盖（multiple definition）。
CONFIG += object_parallel_to_source
TEMPLATE = app
TARGET = cloud-media-client

# 32 位进程默认只有 2GB 用户态地址空间：1080p 解码每帧数 MB，加上大窗口的
# 光栅后备存储，很容易撞墙（CreateDIBSection / malloc 失败 → 进程异常退出）。
# LARGE_ADDRESS_AWARE 让 64 位 Windows 上的 32 位 exe 用满 4GB。
win32-g++:contains(QT_ARCH, i386) {
    QMAKE_LFLAGS += -Wl,--large-address-aware
}

# 客户端源码根（用 "ui/MainWindow.h" 这种写法）
INCLUDEPATH += $$PWD

# ---------------------------------------------------------------------------
#  客户端源码
# ---------------------------------------------------------------------------
SOURCES += \
    app/main.cpp \
    app/smoke.cpp \
    netapi/FrameAssembler.cpp \
    netapi/NetWorker.cpp \
    netapi/NetClient.cpp \
    transfer/TransferQueue.cpp \
    transfer/FolderUploader.cpp \
    sqlapi/TaskStore.cpp \
    shareapi/ShareFlow.cpp \
    recycleapi/RecycleFlow.cpp \
    ui/MainWindow.cpp \
    ui/LoginDialog.cpp \
    ui/FileTableModel.cpp \
    ui/TransferPane.cpp \
    ui/PlayerPane.cpp \
    ui/VideoSurface.cpp \
    ui/GlVideoSurface.cpp \
    ui/PainterVideoSurface.cpp \
    ui/Toast.cpp \
    ui/EmptyState.cpp \
    ui/ShareDialog.cpp \
    ui/RecycleDialog.cpp \
    ui/RegisterDialog.cpp \
    player/PlayerCore.cpp \
    player/PacketQueue.cpp \
    player/GrowingFileIO.cpp

HEADERS += \
    app/smoke.h \
    netapi/FrameAssembler.h \
    netapi/NetWorker.h \
    netapi/NetClient.h \
    transfer/TransferTask.h \
    transfer/TransferQueue.h \
    transfer/FolderUploader.h \
    sqlapi/TaskStore.h \
    shareapi/ShareFlow.h \
    recycleapi/RecycleFlow.h \
    ui/MainWindow.h \
    ui/LoginDialog.h \
    ui/FileTableModel.h \
    ui/TransferPane.h \
    ui/PlayerPane.h \
    ui/VideoSurface.h \
    ui/GlVideoSurface.h \
    ui/PainterVideoSurface.h \
    ui/Letterbox.h \
    ui/Toast.h \
    ui/EmptyState.h \
    ui/ShareDialog.h \
    ui/RecycleDialog.h \
    ui/RegisterDialog.h \
    player/PlayerCore.h \
    player/PacketQueue.h \
    player/GrowingFileIO.h

RESOURCES += ui/resources.qrc

# 服务端地址配置（可选）：在 Qt Creator 的项目树里双击即可改 host/port，
# 构建后会被拷到 exe 同目录（见文末 QMAKE_POST_LINK）。
DISTFILES += ../client.conf

# ---------------------------------------------------------------------------
#  公共层（协议 + 基础设施；纯 C++11，不含 Qt/socket）
# ---------------------------------------------------------------------------
COMMON = $$PWD/../common
INCLUDEPATH += $$COMMON/include
INCLUDEPATH += $$COMMON/protocol/include

SOURCES += \
    $$COMMON/protocol/codec.cpp \
    $$COMMON/protocol/serialize.cpp \
    $$COMMON/protocol/version.cpp \
    $$COMMON/protocol/messages/business_messages.cpp \
    $$COMMON/base/buffer.cpp \
    $$COMMON/base/crc32.cpp \
    $$COMMON/base/error_code.cpp \
    $$COMMON/base/md5.cpp \
    $$COMMON/base/time_util.cpp \
    $$COMMON/base/version.cpp \
    $$COMMON/config/ini_config.cpp

# 注意：这里的 SOURCES 是手写清单，与 common/CMakeLists.txt 的 cm_common 不同步。
# 新增对 common/ 下 .cpp 的调用时，必须同时补到这里，否则 CMake 能过、qmake 会
# 在链接期报 undefined reference（client/app/main.cpp 用 IniConfig 时踩过）。

# ---------------------------------------------------------------------------
#  播放器依赖：FFmpeg 4.2.2 + SDL2 2.0.10（32 位）
#  SDL_MAIN_HANDLED 已在 PlayerCore.cpp 内部 #define，这里无需重复。
# ---------------------------------------------------------------------------
PLAYER_DEPS = $$PWD/player/deps
INCLUDEPATH += $$PLAYER_DEPS/ffmpeg/include
INCLUDEPATH += $$PLAYER_DEPS/sdl2/include

LIBS += -L$$PLAYER_DEPS/ffmpeg/lib -lavformat -lavcodec -lavutil -lswscale -lswresample
LIBS += -L$$PLAYER_DEPS/sdl2/lib -lSDL2

# ---------------------------------------------------------------------------
#  输出目录
# ---------------------------------------------------------------------------
DESTDIR = $$PWD/../build-qmake/bin

# ---------------------------------------------------------------------------
#  构建后把播放器 DLL 拷到 exe 旁
#  （FFmpeg/SDL 不进 PATH，必须和 exe 同目录，否则启动即失败）
#  Qt DLL 由 Qt Creator 的运行环境提供；要独立发布再跑一次：
#      windeployqt --release --no-compiler-runtime <exe>
# ---------------------------------------------------------------------------
win32 {
    # 配置文件一起拷到 exe 旁，否则运行时找不到 client.conf，会退回内置默认地址
    CONF_FILE = $$PWD/../client.conf
    exists($$CONF_FILE) {
        QMAKE_POST_LINK += $$QMAKE_COPY /Y $$shell_quote($$shell_path($$CONF_FILE)) $$shell_quote($$shell_path($$DESTDIR)) $$escape_expand(\\n\\t)
    }

    PLAYER_DLL_DIR = $$PLAYER_DEPS/dll
    exists($$PLAYER_DLL_DIR) {
        PLAYER_DLLS = $$files($$PLAYER_DLL_DIR/*.dll)
        for(dll, PLAYER_DLLS) {
            QMAKE_POST_LINK += $$QMAKE_COPY /Y $$shell_quote($$shell_path($$dll)) $$shell_quote($$shell_path($$DESTDIR)) $$escape_expand(\n\t)
        }
    }
}
