QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

include("./netapi/netapi.pri")
INCLUDEPATH += ./netapi/

include(./md5/md5.pri)
INCLUDEPATH += ./md5/

include(./sqlapi/sqlapi.pri)
INCLUDEPATH += ./sqlapi/


SOURCES += \
    ckernel.cpp \
    logindialog.cpp \
    main.cpp \
    maindialog.cpp \
    mytablewidgetitem.cpp

HEADERS += \
    ckernel.h \
    common.h \
    logindialog.h \
    maindialog.h \
    mytablewidgetitem.h

FORMS += \
    logindialog.ui \
    maindialog.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    resource.qrc

DISTFILES += \
    images/Cancel_32x32.png \
    images/CheckBox_32x32.png \
    images/addFile.png \
    images/check.png \
    images/delete.png \
    images/file.png \
    images/folder.png \
    images/logo.ico \
    images/pause.png \
    images/play.png \
    images/search.png \
    images/share.png \
    images/transmit.ico
