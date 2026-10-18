#ifndef CSERVER_H
#define CSERVER_H

#include <QObject>
#include <INetMediator.h>
#include "packdef.h"
#include "csqlite.h"

// 网盘服务器核心类：
// 监听 8004 端口，处理客户端的 注册 / 登录 请求
// 登录成功后对客户端自动发出的 列表请求 返回空列表，避免客户端空等
class CServer : public QObject
{
    Q_OBJECT
public:
    explicit CServer(QObject *parent = nullptr);
    ~CServer();

public slots:
    // 网络数据到达（由中介者抛出）
    void slot_dealData(unsigned int lSendIP, char *buf, int nlen);

private:
    // 协议处理
    void dealRegisterRq(unsigned int lSendIP, char *buf, int nlen);   // 注册
    void dealLoginRq(unsigned int lSendIP, char *buf, int nlen);      // 登录
    void dealGetFileInfoRq(unsigned int lSendIP, char *buf, int nlen);// 文件列表(空)
    void dealMyShareRq(unsigned int lSendIP, char *buf, int nlen);    // 我的分享(空)

    // 初始化用户数据库
    void initDatabase();

private:
    INetMediator *m_tcpServer;  // 服务端网络中介者
    CSqlite      *m_sql;        // 用户数据库
};

#endif // CSERVER_H
