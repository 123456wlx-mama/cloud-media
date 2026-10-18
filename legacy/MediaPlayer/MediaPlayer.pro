QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    PacketQueue.cpp \
    main.cpp \
    playerdialog.cpp \
    videoplayer.cpp \
    videoslider.cpp

HEADERS += \
    PacketQueue.h \
    playerdialog.h \
    videoplayer.h \
    videoslider.h

FORMS += \
    playerdialog.ui

# 引入 OpenGL 视频渲染模块(glrender/glvideowidget.*)
include($$PWD/glrender/glrender.pri)

# SDL 解压出来有可能多嵌套一层目录, 这里自动适配
SDL_DIR = $$PWD/SDL2-2.0.10
exists($$SDL_DIR/SDL2-2.0.10/include/SDL.h): SDL_DIR = $$PWD/SDL2-2.0.10/SDL2-2.0.10

INCLUDEPATH += $$PWD/ffmpeg-4.2.2/include\
               $$SDL_DIR/include

LIBS += $$PWD/ffmpeg-4.2.2/lib/avcodec.lib\
        $$PWD/ffmpeg-4.2.2/lib/avdevice.lib\
        $$PWD/ffmpeg-4.2.2/lib/avfilter.lib\
        $$PWD/ffmpeg-4.2.2/lib/avformat.lib\
        $$PWD/ffmpeg-4.2.2/lib/avutil.lib\
        $$PWD/ffmpeg-4.2.2/lib/postproc.lib\
        $$PWD/ffmpeg-4.2.2/lib/swresample.lib\
        $$PWD/ffmpeg-4.2.2/lib/swscale.lib\
        $$SDL_DIR/lib/x86/SDL2.lib




# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
