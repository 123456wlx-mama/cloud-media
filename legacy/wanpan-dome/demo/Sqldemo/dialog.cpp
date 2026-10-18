#include "dialog.h"
#include "ui_dialog.h"

Dialog::Dialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::Dialog)
{
    ui->setupUi(this);
        InitDatabase(1);
}

Dialog::~Dialog()
{
    delete ui;
}
#include<QDir>
#include<QDebug>

void Dialog::InitDatabase(int id)
{
m_sql=new CSqlite;
    //首先 找到exe 去同级目录 /database/id.db
    QString path = QCoreApplication::applicationDirPath() + "/database/";
    //先看路径是否存在 要不要创建
    QDir dir;
    if( !dir.exists( path ) ){
        dir.mkdir( path );
    }
    path = path + QString("%1.db").arg( id );
    //查看有没有这个文件
    QFileInfo info( path );
    if( info.exists() )
    {
        //有直接加载
        //连接
        m_sql->ConnectSql( path );

        //测试 读取数据
//        QString sqlbuf = "select count(*) from t_upload;";
//        QStringList lst;
//        m_sql->SelectSql( sqlbuf , 1 , lst );
//        qDebug() << "upload item count:" << lst.front();
//        lst.clear();

//        sqlbuf = "select count(*) from t_download;";
//        m_sql->SelectSql( sqlbuf , 1 , lst );
//        qDebug() << "download item count:" << lst.front();
//        lst.clear();
        QList<FileInfo> uploadTaskList;
        QList<FileInfo>  downloadTaskList;

        slot_getUploadTask( uploadTaskList );
        slot_getDownloadTask( downloadTaskList );

    }
    else{
        //没有 创建数据库文件
        QFile file( path );
        if( !file.open( QIODevice::WriteOnly ) ) return;
        file.close();

        //连接数据库
        m_sql->ConnectSql( path );
        //创建表
        QString sqlbuf = "create table t_upload(timestamp int ,f_id int ,f_name varchar(260),f_dir varchar(260),f_time varchar(60),f_size int,f_md5 varchar(60),f_type varchar(60),f_absolutePath varchar(260));";
        m_sql->UpdateSql(sqlbuf);

        sqlbuf = "create table t_download(timestamp int ,f_id int ,f_name varchar(260),f_dir varchar(260),f_time varchar(60),f_size int,f_md5 varchar(60),f_type varchar(60),f_absolutePath varchar(260));";
        m_sql->UpdateSql(sqlbuf);
    }
}

void Dialog::slot_writeUploadTask(FileInfo &info)
{
    QString sqlbuf = QString("insert into t_upload values( %1 , %2 , '%3' , '%4' , '%5' , %6 , '%7' ,'%8' ,'%9' );")
        .arg(info.timestamp).arg(info.fileid).arg(info.name).arg(info.dir).arg(info.time)
        .arg(info.size).arg(info.md5).arg(info.type).arg(info.absolutePath);
    m_sql->UpdateSql( sqlbuf );
}

void Dialog::slot_writeDownloadTask(FileInfo &info)
{
    QString sqlbuf = QString("insert into t_download values( %1 , %2 , '%3' , '%4' , '%5' , %6 , '%7' ,'%8' ,'%9' );")
        .arg(info.timestamp).arg(info.fileid).arg(info.name).arg(info.dir).arg(info.time)
        .arg(info.size).arg(info.md5).arg(info.type).arg(info.absolutePath);
    m_sql->UpdateSql( sqlbuf );
}

void Dialog::slot_deleteUploadTask(FileInfo &info)
{
    QString sqlbuf = QString("delete from t_upload where timestamp = %1;").arg(info.timestamp);
    m_sql->UpdateSql( sqlbuf );
}

void Dialog::slot_deleteDownloadTask(FileInfo &info)
{
    QString sqlbuf = QString("delete from t_download where timestamp = %1 and f_absolutePath = '%2'; ")
        .arg(info.timestamp).arg(info.absolutePath);
    m_sql->UpdateSql( sqlbuf );
}

void Dialog::slot_getUploadTask(QList<FileInfo> &infoList)
{
    //获取所有的任务
    QString sqlbuf = "select * from t_upload;";
    QStringList lst;
    m_sql->SelectSql( sqlbuf , 9 , lst );
    /*timestamp int ,f_id int ,f_name varchar(260),f_dir varchar(260),f_time varchar(60),f_size int,f_md5 varchar(60),f_type varchar(60),f_absolutePath varchar(260)*/
    while(lst.size() != 0){
        FileInfo info;
        info.timestamp = QString(lst.front()).toInt(); lst.pop_front();
        info.fileid = QString(lst.front()).toInt(); lst.pop_front();
        info.name = lst.front(); lst.pop_front();
        info.dir = lst.front(); lst.pop_front();
        info.time = lst.front(); lst.pop_front();
        info.size = QString(lst.front()).toInt(); lst.pop_front();
        info.md5 = lst.front(); lst.pop_front();
        info.type = lst.front(); lst.pop_front();
        info.absolutePath = lst.front(); lst.pop_front();

        infoList.push_back( info );
    }
}

void Dialog::slot_getDownloadTask(QList<FileInfo> &infoList)
{
    //获取所有的任务
    QString sqlbuf = "select * from t_download;";
    QStringList lst;
    m_sql->SelectSql( sqlbuf , 9 , lst );
    /*timestamp int ,f_id int ,f_name varchar(260),f_dir varchar(260),f_time varchar(60),f_size int,f_md5 varchar(60),f_type varchar(60),f_absolutePath varchar(260)*/
    while(lst.size() != 0){
        FileInfo info;
        info.timestamp = QString(lst.front()).toInt(); lst.pop_front();
        info.fileid = QString(lst.front()).toInt(); lst.pop_front();
        info.name = lst.front(); lst.pop_front();
        info.dir = lst.front(); lst.pop_front();
        info.time = lst.front(); lst.pop_front();
        info.size = QString(lst.front()).toInt(); lst.pop_front();
        info.md5 = lst.front(); lst.pop_front();
        info.type = lst.front(); lst.pop_front();
        info.absolutePath = lst.front(); lst.pop_front();

        infoList.push_back( info );
    }
}


// 客户端数据库 id.db
//表 正在上传
/*
create table t_upload(
timestamp int ,
f_id int ,
f_name varchar(260),
f_dir varchar(260),
f_time varchar(60),
f_size int,
f_md5 varchar(60),
f_type varchar(60),
f_absolutePath(260)
);
*/
//表 正在下载
/*
create table t_download(
timestamp int ,
f_id int ,
f_name varchar(260),
f_dir varchar(260),
f_time varchar(60),
f_size int,
f_md5 varchar(60),
f_type varchar(60),
f_absolutePath(260)
);
*/
