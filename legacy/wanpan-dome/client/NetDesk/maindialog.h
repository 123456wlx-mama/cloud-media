#ifndef MAINDIALOG_H
#define MAINDIALOG_H

#include <QDialog>
#include <QCloseEvent>
#include<QMenu>//菜单
#include"common.h"
#include"mytablewidgetitem.h"
// 前置UI命名空间声明
namespace Ui {
class MainDialog;
}
class CKernel;
class QTableWidget;
class MainDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MainDialog(QWidget *parent = nullptr);
    ~MainDialog() override;

signals:
    // 自定义关闭信号：点击确认退出时发送，通知核心类释放资源
    void SIG_close();

    //什么绝对路径的文件，上传到什么目录下
    void SIG_uploadFile( QString path, QString dir);
    //上传什么路径的文件夹，到什么目录下：
    void SIG_uploadFolder( QString path, QString dir);

    //什么文件id，什么目录下的文件 下载
    void SIG_downloadFile( int fileid , QString dir);
    //什么文件id，什么目录下的文件夹 下载
    void SIG_downloadFolder( int fileid , QString dir);
    //什么路径下创建什么名字的文件夹：
    void SIG_addFolder(QString name ,QString dir );
    //改变路径：
    void SIG_changeDir(QString dir);

    //分享 什么目录下面的文件列表
    void SIG_shareFile(QVector<int> fileidArray, QString dir);
    //获取什么分享码的文件 添加到什么目录
    void SIG_getShareByLink(int  code , QString dir);
    //删除什么目录下的 一系列文件  （文件id 数组）
    void SIG_deleteFile(QVector<int> fileidArray, QString dir);
    //设置上传暂停 0 开始 1 暂停
    void SIG_setUploadPause( int timestamp , int isPause );
    //设置下载暂停 0 开始 1 暂停
    void SIG_setDownloadPause( int timestamp , int isPause );



private slots:

    void slot_setInfo(QString name);
    void on_pb_file_clicked();

    void on_pb_transmit_clicked();

    void on_pb_share_clicked();

    void on_pb_addFile_clicked();

    void slot_addFolder( bool flag );
    void slot_uploadFile( bool flag );
    void slot_uploadFolder( bool flag );

    void slot_downloadFile(bool flag);
    void slot_shareFile(bool flag);
    void slot_deleteFile(bool flag);

    void slot_getShare(bool flag);

    void slot_uploadPause(bool flag);
    void slot_uploadResume( bool flag);
    void slot_downloadPause( bool flag);
    void slot_downloadResume( bool flag);
    //批量设置 上传/下载表格 所有行的 暂停(1) 开始(0)  isUpload:true上传表 false下载表
    void slot_setAllPause( QTableWidget * table , int isPause , bool isUpload );

    void slot_insertUploadFile(FileInfo& info);

    void slot_insertDownloadFile(FileInfo& info);
    //插入上传完成
    void slot_insertUploadComplete(FileInfo& info);

    void slot_updateUploadFileProgress(int timestamp, int pos);//pos当前位置

    void slot_updateDownloadFileProgress(int timestamp, int pos);

    void slot_deleteUploadFileByRow(int row);

    void slot_insertFileInfo(FileInfo& info);

    void slot_deleteDownloadFileByRow( int row );
    void slot_insertDownloadComplete( FileInfo& info );

    void slot_insertShareFileInfo( QString name, int size , QString time, int shareLink );

    void on_table_file_cellClicked(int row, int column);

    void on_table_file_customContextMenuRequested(const QPoint &pos);

    void slot_openPath(bool flag);

    void slot_deleteAllFileInfo();

    void slot_deleteAllShareInfo();

    void on_table_file_cellDoubleClicked(int row, int column);

    void on_pb_prew_clicked();

    void on_table_upload_cellClicked(int row, int column);

    void on_table_download_cellClicked(int row, int column);

    bool slot_getDownloadFileInfoByTimestamp(int timestamp,FileInfo &info);

    bool slot_getUploadFileInfoByTimestamp(int timestamp,FileInfo &info);

private:
    //【重点】重写窗口关闭虚函数，拦截右上角叉号关闭事件
    void closeEvent(QCloseEvent *event) override;

private:
    Ui::MainDialog *ui;

    QMenu m_menuAddFile;

    QMenu m_menuFileInfo;//文件信息的菜单

    QMenu m_menuUpload;
    QMenu m_menuDownload;


    friend class CKernel;//调用对方私有
};

#endif // MAINDIALOG_H
