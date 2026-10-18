#ifndef CKERNEL_H
#define CKERNEL_H

#include <QObject>
#include <QString>
#include <INetMediator.h>
#include"packdef.h"
#include"logindialog.h"
#include"common.h"
//#include"maindialog.h"
#include"csqlite.h"
//协议处理类
//类成员函数指针
class CKernel;
typedef void (CKernel::*PFUN)(unsigned int lSendIP , char* buf , int nlen);



// 前置声明窗口类，减少头文件依赖
class MainDialog;
//#define USE_SERVER 1

class CKernel : public QObject
{
    //【重点】使用信号槽必须添加该宏
    Q_OBJECT
private:
    // 1.私有化构造、拷贝构造、析构，禁止外部创建/复制实例
    explicit CKernel(QObject *parent = nullptr);
    CKernel(const CKernel& kernel) = delete; // 禁用拷贝构造
    ~CKernel() = default;
    void setNetPackMap();//映射函数
    void setSystemPath();
signals:
    // 自定义信号区，本类暂无对外信号
    void sig_insertUploadFile(FileInfo &info);
    void SIG_updateUploadFileProgress(int timestamp, int pos);
    void SIG_updateDownloadFileProgress(int timestamp, int pos);
public:
    // 静态方法：获取全局唯一单例对象
    static CKernel* GetInstance();

public slots:
    // 槽函数：接收窗口关闭信号，释放窗口堆内存
    void slot_destory();
    //注册登录响应槽函数：
    void slot_registerCommit(QString tel,QString password,QString name);
    void slot_loginCommit(QString tel,QString password);

    //信号对应的槽函数
    //什么绝对路径的文件，上传到什么目录下
    void slot_uploadFile( QString path, QString dir);

    //上传什么路径的文件夹 ，到什么目录下面
    void slot_uploadFolder( QString path, QString dir );


    void slot_getCurDirFileList();
    //添加槽函数：
    //什么文件id，什么目录下的文件 下载
    void slot_downloadFile( int fileid , QString dir);
    //什么文件id，什么目录下的文件夹 下载
    void slot_downloadFolder( int fileid , QString dir);
    //什么路径下创建什么名字的文件夹：
    void slot_addFolder(QString name ,QString dir );
    //改变路径
    void slot_changeDir(QString dir);
    //分享 什么目录下面的文件列表
    void slot_shareFile(QVector<int> fileidArray, QString dir);
    //获取个人所有分享
    void slot_getMyShare();
    //获取什么分享码的文件 添加到什么目录
    void slot_getShareByLink(int  code , QString dir);
    //删除什么目录下的 一系列文件  （文件id 数组）
    void slot_deleteFile(QVector<int> fileidArray, QString dir);
    //设置上传暂停 0 开始 1 暂停
    void slot_setUploadPause( int timestamp , int isPause );
    //设置下载暂停 0 开始 1 暂停
    void slot_setDownloadPause( int timestamp , int isPause );




    //网络响应槽函数
    void slot_dealClientData(unsigned int lSendIP , char* buf , int nlen);
    //处理登录回复的：
    void slot_dealLoginRs(unsigned int lSendIP , char* buf , int nlen);
    //处理注册回复：
    void slot_dealRegisterRs(unsigned int lSendIP , char* buf , int nlen);
    //上传文件回复：
    void slot_dealUploadFileRs(unsigned int lSendIP , char* buf , int nlen);
    //处理文件块回复：
    void slot_dealFileContentRs(unsigned int lSendIP , char* buf , int nlen);
    void slot_dealGetFileInfoRs(unsigned int lSendIP , char* buf , int nlen);
    void slot_dealFileHeaderRq(unsigned int lSendIP , char* buf , int nlen);
    void slot_dealFileContentRq(unsigned int lSendIP, char* buf, int nlen);
    void slot_dealAddFolderRs(unsigned int lSendIP, char* buf, int nlen);
    void slot_dealQuickUploadRs(unsigned int lSendIP, char* buf, int nlen);
    void slot_dealShareFileRs(unsigned int lSendIP, char* buf, int nlen);
    void slot_dealMyShareRs(unsigned int lSendIP, char* buf, int nlen);
    void slot_dealGetShareRs(unsigned int lSendIP, char* buf, int nlen);
    void slot_dealFolderHeadRq(unsigned int lSendIP, char* buf, int nlen);
    void slot_dealDeleteFileRs(unsigned int lSendIP, char* buf, int nlen);
    void slot_dealContinueUploadRs(unsigned int lSendIP, char* buf, int nlen);
#ifdef USE_SERVER

    void slot_dealServerData(unsigned int lSendIP , char* buf , int nlen);
#endif

private:

    void SendData(char* buf,int len);//发送数据函数

private:
    MainDialog *m_mainDialog; // 主窗口指针（堆创建）
    LoginDialog*m_loginDialog;
    QString m_ip;     // 服务器IP，从ini配置读取
    QString m_port;   // 服务器端口，从ini配置读取

    QString m_name;
    int m_id;
    QString m_curDir;//网盘当前的目录
    QString m_sysPath;//默认存储的系统路径---绝对路径，exe同级下面 NetDisk文件夹下面

    //退出标志：
    bool m_quit;
    //数据库
    CSqlite * m_sql;

    INetMediator *m_tcpClient;
#ifdef USE_SERVER
    INetMediator *m_tcpServer;
#endif

    // 加载ini配置文件的函数
    void loadIniFile();

    // key 时间戳 hhmsszzz int 21 xxxx xxxx value 文件信息
    std::map<int, FileInfo> m_mapTimestampToFileInfo;

    //定义映射数组：
    PFUN m_netPackMap[_DEF_PACK_COUNT];
private:
    void InitDatabase(int id);
    //缓存上传的任务
    void slot_writeUploadTask( FileInfo & info );
    //缓存下载的任务
    void slot_writeDownloadTask( FileInfo & info );
    //完成任务，删除上传记录
    void slot_deleteUploadTask( FileInfo & info );
    //完成任务，删除下载任务
    void slot_deleteDownloadTask( FileInfo & info );
    //添加上传任务
    void slot_getUploadTask( QList<FileInfo> &infoList );
    //加载下载任务
    void slot_getDownloadTask( QList<FileInfo> &infoList );

};

#endif // CKERNEL_H
