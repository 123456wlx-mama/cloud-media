QT       += core sql
QT       -= gui

greaterThan(QT_MAJOR_VERSION, 4): QT += network

CONFIG += c++11 console
CONFIG -= app_bundle

TEMPLATE = app
TARGET = NetDeskServer

# 复用客户端的网络层、数据库层
include(../NetDesk/netapi/netapi.pri)
INCLUDEPATH += ../NetDesk/netapi/

include(../NetDesk/sqlapi/sqlapi.pri)
INCLUDEPATH += ../NetDesk/sqlapi/

SOURCES += \
    main.cpp \
    cserver.cpp

HEADERS += \
    cserver.h
