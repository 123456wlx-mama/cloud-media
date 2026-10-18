#include "cserver.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QDebug>

CServer::CServer(QObject *parent)
    : QObject(parent)
    , m_tcpServer(nullptr)
    , m_sql(nullptr)
{
    // 初始化用户数据库
    initDatabase();

    // 创建服务端中介者
    m_tcpServer = new TcpServerMediator;

    // 连接数据到达信号
    connect(m_tcpServer,
            SIGNAL(SIG_ReadyData(uint,char*,int)),
            this,
            SLOT(slot_dealData(uint,char*,int)));

    // 监听所有网卡的 8004 端口（与客户端 config.ini 的端口一致）
    if (!m_tcpServer->OpenNet("0.0.0.0", 8004))
    {
        qDebug() << "[服务器] 监听 8004 端口失败！请检查端口是否被占用。";
    }
    else
    {
        qDebug() << "[服务器] 启动成功，监听端口 8004，等待客户端连接...";
    }
}

CServer::~CServer()
{
    if (m_tcpServer)
    {
        m_tcpServer->CloseNet();
        delete m_tcpServer;
        m_tcpServer = nullptr;
    }
    if (m_sql)
    {
        delete m_sql;
        m_sql = nullptr;
    }
}

// 初始化数据库：exe 同级目录 /NetDeskServer.db
void CServer::initDatabase()
{
    m_sql = new CSqlite;

    QString path = QCoreApplication::applicationDirPath() + "/NetDeskServer.db";

    // 文件不存在则先创建
    QFileInfo info(path);
    if (!info.exists())
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
        {
            qDebug() << "[服务器] 创建数据库文件失败:" << path;
            return;
        }
        file.close();
    }

    m_sql->ConnectSql(path);

    // 用户表：id 自增主键，手机号、密码(客户端传来的MD5)、昵称
    QString sqlbuf = "create table if not exists t_user("
                     "id integer primary key autoincrement,"
                     "tel varchar(40),"
                     "password varchar(40),"
                     "name varchar(40));";
    if (!m_sql->UpdateSql(sqlbuf))
    {
        qDebug() << "[服务器] 创建用户表失败！";
    }
}

// 网络数据到达：按协议类型分发
void CServer::slot_dealData(unsigned int lSendIP, char *buf, int nlen)
{
    if (buf == nullptr || nlen < (int)sizeof(int))
    {
        delete[] buf;
        return;
    }

    int type = *(int*)buf;

    switch (type)
    {
    case _DEF_PACK_REGISTER_RQ:
        dealRegisterRq(lSendIP, buf, nlen);
        break;
    case _DEF_PACK_LOGIN_RQ:
        dealLoginRq(lSendIP, buf, nlen);
        break;
    case _DEF_PACK_GET_FILE_INFO_RQ:
        dealGetFileInfoRq(lSendIP, buf, nlen);
        break;
    case _DEF_PACK_MY_SHARE_RQ:
        dealMyShareRq(lSendIP, buf, nlen);
        break;
    default:
        qDebug() << "[服务器] 未处理的协议类型:" << type;
        break;
    }

    // 回收接收缓冲区
    delete[] buf;
}

// 处理注册请求
void CServer::dealRegisterRq(unsigned int lSendIP, char *buf, int nlen)
{
    Q_UNUSED(nlen);
    STRU_REGISTER_RQ *rq = (STRU_REGISTER_RQ *)buf;

    qDebug() << "[服务器] 收到注册请求 tel:" << rq->tel << " name:" << rq->name;

    STRU_REGISTER_RS rs;

    // 1.手机号是否已存在
    QStringList lst;
    QString sqlbuf = QString("select count(*) from t_user where tel = '%1';")
            .arg(QString::fromUtf8(rq->tel));
    m_sql->SelectSql(sqlbuf, 1, lst);
    if (!lst.isEmpty() && lst.front().toInt() > 0)
    {
        rs.result = tel_is_exist;
        m_tcpServer->SendData(lSendIP, (char*)&rs, sizeof(rs));
        return;
    }

    // 2.昵称是否已存在
    lst.clear();
    sqlbuf = QString("select count(*) from t_user where name = '%1';")
            .arg(QString::fromUtf8(rq->name));
    m_sql->SelectSql(sqlbuf, 1, lst);
    if (!lst.isEmpty() && lst.front().toInt() > 0)
    {
        rs.result = name_is_exist;
        m_tcpServer->SendData(lSendIP, (char*)&rs, sizeof(rs));
        return;
    }

    // 3.写入用户表（密码为客户端传来的MD5串）
    sqlbuf = QString("insert into t_user(tel,password,name) values('%1','%2','%3');")
            .arg(QString::fromUtf8(rq->tel))
            .arg(QString::fromUtf8(rq->password))
            .arg(QString::fromUtf8(rq->name));
    if (!m_sql->UpdateSql(sqlbuf))
    {
        rs.result = tel_is_exist; // 数据库写入失败，复用失败提示
    }
    else
    {
        rs.result = register_success;
    }

    m_tcpServer->SendData(lSendIP, (char*)&rs, sizeof(rs));
}

// 处理登录请求
void CServer::dealLoginRq(unsigned int lSendIP, char *buf, int nlen)
{
    Q_UNUSED(nlen);
    STRU_LOGIN_RQ *rq = (STRU_LOGIN_RQ *)buf;

    qDebug() << "[服务器] 收到登录请求 tel:" << rq->tel;

    STRU_LOGIN_RS rs;

    // 1.按手机号查用户
    QStringList lst;
    QString sqlbuf = QString("select id,password,name from t_user where tel = '%1';")
            .arg(QString::fromUtf8(rq->tel));
    m_sql->SelectSql(sqlbuf, 3, lst);

    if (lst.isEmpty())
    {
        // 手机号不存在
        rs.result = tel_not_exist;
        m_tcpServer->SendData(lSendIP, (char*)&rs, sizeof(rs));
        return;
    }

    // 2.校验密码（客户端发来的就是MD5串，直接比对）
    QString id       = lst[0];
    QString password = lst[1];
    QString name     = lst[2];

    if (password != QString::fromUtf8(rq->password))
    {
        rs.result = password_error;
        m_tcpServer->SendData(lSendIP, (char*)&rs, sizeof(rs));
        return;
    }

    // 3.登录成功，返回用户id和昵称
    rs.result = login_success;
    rs.userid = id.toInt();
    strcpy(rs.name, name.toUtf8().data());

    m_tcpServer->SendData(lSendIP, (char*)&rs, sizeof(rs));
}

// 处理文件列表请求：当前服务器未实现文件存储，返回空列表
void CServer::dealGetFileInfoRq(unsigned int lSendIP, char *buf, int nlen)
{
    Q_UNUSED(nlen);
    STRU_GET_FILE_INFO_RQ *rq = (STRU_GET_FILE_INFO_RQ *)buf;

    STRU_GET_FILE_INFO_RS *rs = (STRU_GET_FILE_INFO_RS *)malloc(sizeof(STRU_GET_FILE_INFO_RS));
    rs->init();
    strcpy(rs->dir, rq->dir);   // 原样返回目录，客户端用它做校验
    rs->count = 0;

    m_tcpServer->SendData(lSendIP, (char*)rs, sizeof(STRU_GET_FILE_INFO_RS));
    free(rs);
}

// 处理我的分享请求：返回空列表
void CServer::dealMyShareRq(unsigned int lSendIP, char *buf, int nlen)
{
    Q_UNUSED(buf);
    Q_UNUSED(nlen);

    STRU_MY_SHARE_RS *rs = (STRU_MY_SHARE_RS *)malloc(sizeof(STRU_MY_SHARE_RS));
    rs->init();
    rs->itemCount = 0;

    m_tcpServer->SendData(lSendIP, (char*)rs, sizeof(STRU_MY_SHARE_RS));
    free(rs);
}
