# OpenGL 视频渲染模块(工程包含文件)
# 引入方式: 在 MediaPlayer.pro 里加一行 include($$PWD/glrender/glrender.pri)
# 好处: 头文件/源文件/依赖库都写在这里, 主工程只 include 一次, 不会漏配置

QT += opengl

INCLUDEPATH += $$PWD

HEADERS += $$PWD/glvideowidget.h
SOURCES += $$PWD/glvideowidget.cpp
