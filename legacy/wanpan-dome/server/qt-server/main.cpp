#include <QCoreApplication>
#include <QDebug>
#include "cserver.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    CServer server;

    qDebug() << "NetDesk 服务器运行中... (监听 0.0.0.0:8004)";

    return a.exec();
}
