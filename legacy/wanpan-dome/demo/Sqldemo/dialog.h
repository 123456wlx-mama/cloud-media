#ifndef DIALOG_H
#define DIALOG_H

#include <QDialog>
#include"common.h"
#include"csqlite.h"

QT_BEGIN_NAMESPACE
namespace Ui { class Dialog; }
QT_END_NAMESPACE

class Dialog : public QDialog
{
    Q_OBJECT

public:
    Dialog(QWidget *parent = nullptr);
    ~Dialog();

    //登陆之后，初始化数据
    void InitDatabase( int id );
public slots:
    //缓存上传的任务
    void slot_writeUploadTask( FileInfo & info );
    //缓存下载的任务
    void slot_writeDownloadTask( FileInfo & info );
    //完成任务，删除上传记录
    void slot_deleteUploadTask( FileInfo & info );
    //完成任务，删除下载任务
    void slot_deleteDownloadTask( FileInfo & info );
    //加载上传任务
    void slot_getUploadTask( QList<FileInfo> &infoList );
    //加载下载任务
    void slot_getDownloadTask( QList<FileInfo> &infoList );

private:
    Ui::Dialog *ui;

    CSqlite * m_sql;
};
#endif // DIALOG_H
