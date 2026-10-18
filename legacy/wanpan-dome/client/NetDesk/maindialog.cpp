#include "maindialog.h"
#include "ui_maindialog.h"
#include <QMessageBox>
#include <QDebug>
#include <QTimer>
#include <QDesktopServices>
#include <QFileInfo>
#include <QUrl>

MainDialog::MainDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::MainDialog)
{
    ui->setupUi(this); // 加载ui界面控件
    //默认文件分页：
    ui->sw_page->setCurrentIndex(0);
    //传输默认分页：
    ui->tw_transmit->setCurrentIndex(2);
    //设置标题栏
    this->setWindowTitle("我的网盘");
    //设置 最小最大化
    this->setWindowFlags( Qt::WindowMinMaxButtonsHint|Qt::WindowCloseButtonHint);

    //首先定义菜单项
    QAction * action_addFolder = new QAction(QIcon(":/images/folder.png"),"新建文件夹");
    QAction * action_uploadFile = new QAction("上传文件");
    QAction * action_uploadFolder = new QAction("上传文件夹");
    //添加菜单项
    m_menuAddFile.addAction( action_addFolder );
    m_menuAddFile.addSeparator(); //加入分隔符
    m_menuAddFile.addAction( action_uploadFile );
    m_menuAddFile.addAction( action_uploadFolder );
    //建立连接：
    connect( action_addFolder , SIGNAL(triggered(bool)) ,
             this , SLOT(slot_addFolder(bool)) );
    connect( action_uploadFile ,SIGNAL(triggered(bool)) ,
             this , SLOT(slot_uploadFile(bool)) );
    connect( action_uploadFolder ,SIGNAL(triggered(bool)) ,
             this , SLOT(slot_uploadFolder(bool)) );

    // 创建右键菜单的QAction菜单项
    QAction *action_downloadFile = new QAction("下载文件");
    QAction *action_shareFile = new QAction("分享文件");
    QAction *action_deleteFile = new QAction("删除文件");
    QAction *action_getShare = new QAction("获取分享");//获取分享到本地目录

    // 往右键菜单添加菜单项
    m_menuFileInfo.addAction(action_addFolder);
    m_menuFileInfo.addSeparator();        // 加入分隔符
    m_menuFileInfo.addAction(action_downloadFile);
    m_menuFileInfo.addAction(action_shareFile);
    m_menuFileInfo.addAction(action_deleteFile);
    m_menuFileInfo.addAction("收藏");
    m_menuFileInfo.addSeparator();        // 加入分隔符
    m_menuFileInfo.addAction(action_getShare);


    // 绑定Action点击信号到槽函数（这里槽名暂时空着，后续补全）
    connect(action_downloadFile, SIGNAL(triggered(bool)), this, SLOT(slot_downloadFile(bool)));
    connect(action_shareFile, SIGNAL(triggered(bool)), this, SLOT(slot_shareFile(bool)));
    connect(action_deleteFile, SIGNAL(triggered(bool)), this, SLOT(slot_deleteFile(bool)));
    connect(action_getShare, SIGNAL(triggered(bool)), this, SLOT(slot_getShare(bool)));

    //      ui->table_download;
    //      ui->table_upload;

    //添加右键显示菜单  lambda表达式  匿名函数
    // [ ] 捕捉列表 ( ) 函数参数列表  { } 函数体
    //      connect( ui->table_download , SIGNAL() , this , SLOT() );
    //ui文件中 table_upload 漏配 contextMenuPolicy, 导致右键不会触发信号、菜单弹不出来
    ui->table_upload->setContextMenuPolicy( Qt::CustomContextMenu );

    connect( ui->table_download , &QTableWidget::customContextMenuRequested,
             this , [ this ]( QPoint ){  this->m_menuDownload.exec( QCursor::pos() ); });
    connect( ui->table_upload , &QTableWidget::customContextMenuRequested,
             this , [ this ]( QPoint ){  this->m_menuUpload.exec( QCursor::pos() ); });
    //添加菜单项
    QAction *actionUploadPause = new QAction("暂停");
    QAction *actionUploadResume = new QAction("开始");
    QAction *actionDownloadPause = new QAction("暂停");
    QAction *actionDownloadResume = new QAction("开始");

    QAction *actionUploadPauseAll = new QAction("全部暂停");
    QAction *actionUploadResumeAll = new QAction("全部开始");
    QAction *actionDownloadPauseAll = new QAction("全部暂停");
    QAction *actionDownloadResumeAll = new QAction("全部开始");

    m_menuUpload.addAction( actionUploadPause );
    m_menuUpload.addAction( actionUploadResume );
    m_menuUpload.addAction( actionUploadPauseAll );
    m_menuUpload.addAction( actionUploadResumeAll );

    m_menuDownload.addAction( actionDownloadPause );
    m_menuDownload.addAction( actionDownloadResume );
    m_menuDownload.addAction( actionDownloadPauseAll );
    m_menuDownload.addAction( actionDownloadResumeAll );

    connect( actionUploadPause , SIGNAL(triggered(bool)) ,
        this , SLOT( slot_uploadPause(bool) ) );
    connect( actionUploadResume , SIGNAL(triggered(bool)) ,
        this , SLOT( slot_uploadResume(bool) ) );
    connect( actionDownloadPause , SIGNAL(triggered(bool)) ,
        this , SLOT( slot_downloadPause(bool) ) );
    connect( actionDownloadResume , SIGNAL(triggered(bool)) ,
        this , SLOT( slot_downloadResume(bool) ) );

    //全部暂停 / 全部开始 : 遍历所有行, 不要求勾选
    connect( actionUploadPauseAll , &QAction::triggered , this ,
             [ this ]( bool ){ slot_setAllPause( ui->table_upload , 1 , true ); } );
    connect( actionUploadResumeAll , &QAction::triggered , this ,
             [ this ]( bool ){ slot_setAllPause( ui->table_upload , 0 , true ); } );
    connect( actionDownloadPauseAll , &QAction::triggered , this ,
             [ this ]( bool ){ slot_setAllPause( ui->table_download , 1 , false ); } );
    connect( actionDownloadResumeAll , &QAction::triggered , this ,
             [ this ]( bool ){ slot_setAllPause( ui->table_download , 0 , false ); } );


}

MainDialog::~MainDialog()
{
    delete ui; // 释放ui自动生成的控件内存
}

void MainDialog::slot_setInfo(QString name)
{
    ui->pb_name->setText(name);
}

// 拦截窗口关闭事件
void MainDialog::closeEvent(QCloseEvent *event)
{
    // 弹出询问弹窗：标题、提示文本
    int ret = QMessageBox::question(this, "退出提示", "是否退出网盘客户端？");
    if (ret == QMessageBox::Yes)
    {
        // 用户确认退出：允许关闭窗口
        event->accept();
        // 发射关闭信号，通知核心类释放窗口资源
        emit SIG_close();
    }
    else
    {
        // 用户取消：忽略关闭，窗口保持打开
        event->ignore();
    }
}

//对应maindialong的不同项目
void MainDialog::on_pb_file_clicked()
{
    ui->sw_page->setCurrentIndex(0);
}


void MainDialog::on_pb_transmit_clicked()
{
    ui->sw_page->setCurrentIndex(1);
}


void MainDialog::on_pb_share_clicked()
{
    ui->sw_page->setCurrentIndex(2);
}

//点击添加文件
void MainDialog::on_pb_addFile_clicked()
{
    //弹出菜单：   参数为鼠标的坐标
    m_menuAddFile.exec(QCursor::pos());//pos鼠标的坐标，在该点显示菜单

}
//槽函数
//新建文件夹---------------
#include<QInputDialog>
//#include<QMessageBox>
//#include<QDebug>

void MainDialog::slot_addFolder(bool flag)
{
    qDebug() << __func__;

    //弹出输入窗口，获取文件夹名称
    QString name = QInputDialog::getText(this , "新建文件夹" , "输入名称");

    QString tmp = name;
    //空白符、空字符串、长度超限校验
    if( name.isEmpty() || tmp.remove(" ").isEmpty() || name.length() > 100 )
    {
        QMessageBox::about(this , "提示" , "名字非法");
        return;
    }

    //校验Windows文件名非法字符：\ / : ? * < > | "
    if( name.contains("\\") || name.contains("/") || name.contains(":") ||
        name.contains("?") || name.contains("*") || name.contains("<") ||
        name.contains(">") || name.contains("|") || name.contains("\"") )
    {
        QMessageBox::about(this , "提示" , "名字非法");
        return;
    }

    //TODO：判断同名文件夹是否已经存在

    //获取当前路径，发射新建文件夹信号，传递文件夹名与父目录路径
    QString dir = ui->lb_path->text();
    Q_EMIT SIG_addFolder( name , dir );
}




#include<QFileDialog>
//上传文件
void MainDialog::slot_uploadFile(bool flag)
{
    qDebug() << __func__;
    //弹出窗口 选择文件
    QString path = QFileDialog::getOpenFileName( this , "选择文件" , "./" );
    if( path.isEmpty() ) return;

    //目前上传的有没有 一样的文件  如果是 取消 todo

    //发送信号 核心类处理  传递的信息 ： 上传什么文件到 什么目录下
    QString dir = ui->lb_path->text();

    Q_EMIT SIG_uploadFile( path , dir );//文件名是变化的，文件夹


}




//上传文件夹：
void MainDialog::slot_uploadFolder(bool flag)
{
    qDebug() << __func__;
    //点击 弹出文件选择对话框 选择路径
    QString path = QFileDialog::getExistingDirectory( this , "选择文件夹" , "./");
    //判断非空
    if( path.isEmpty() ) return;
    //过滤  是否正在传 todo

    //发信号 上传什么路径的文件夹 ，到什么目录下面
    Q_EMIT SIG_uploadFolder( path , ui->lb_path->text() );
}


void MainDialog::slot_downloadFile(bool flag)
{
    qDebug() << __func__;
    //遍历列表
    int rows = ui->table_file->rowCount();
    QString dir = ui->lb_path->text();//获取目录
    for( int i = 0 ; i < rows ; ++i){
        //看选中的
        MyTableWidgetItem* item0 = (MyTableWidgetItem*)ui->table_file->item(i,0);
        if( item0 ->checkState() == Qt::Checked ){
            //列表中有这个下载，不能开始 todo 过滤

            //获取类型
            if( item0->m_info.type == "file"){//发信号 下载文件 下载文件夹
                Q_EMIT SIG_downloadFile( item0->m_info.fileid , dir);
            }else{
                Q_EMIT SIG_downloadFolder( item0->m_info.fileid , dir);
            }
        }
    }
}

void MainDialog::slot_shareFile(bool flag)
{
    qDebug() << __func__;
    //申请数组
    QVector<int> array;
    int count = ui->table_file->rowCount();
    //遍历所有项
    for( int i = 0 ; i < count ; ++i ){
        MyTableWidgetItem * item0 = (MyTableWidgetItem *)ui->table_file->item( i , 0 );
        //看是否是打钩的
        if( item0->checkState() == Qt::Checked ){
            //添加到数组里面
            array.push_back( item0->m_info.fileid );
        }
    }
    //发送信号
    Q_EMIT SIG_shareFile( array , ui->lb_path->text() );
}

void MainDialog::slot_deleteFile(bool flag)
{
    qDebug()<<__func__;
    QVector<int> array;
        int count = ui->table_file->rowCount();
        //遍历所有项
        for( int i = 0 ; i < count ; ++i ){
            MyTableWidgetItem * item0 = (MyTableWidgetItem *)ui->table_file->item( i , 0 );
            //看是否是打钩的
            if( item0->checkState() == Qt::Checked ){
                //添加到数组里面
                array.push_back( item0->m_info.fileid );
            }
        }
        //发送信号
        Q_EMIT SIG_deleteFile( array , ui->lb_path->text() );
}

void MainDialog::slot_getShare(bool flag)
{
    qDebug() << __func__;
    //弹窗 输入分享码
    QString txt = QInputDialog::getText( this , "获取分享" , "输入分享码" );
    //过滤
    int code = txt.toInt();
    if( txt.length() != 9 || code < 100000000 || code >= 1000000000 ){
        QMessageBox::about( this , "提示" , "分享码非法");
        return;
    }
    //发送信号 什么目录下面 添加什么分享码的文件
    Q_EMIT SIG_getShareByLink( code , ui->lb_path->text() );
}

void MainDialog::slot_uploadPause(bool flag)
{
    qDebug() << "上传暂停";
    int rows = ui->table_upload->rowCount();
    //遍历表单
    for( int i = 0 ; i < rows ; ++i ){
        MyTableWidgetItem * item0 = (MyTableWidgetItem *)ui->table_upload->item( i , 0 );
        //看是否打钩
        if( item0->checkState() == Qt::Checked ){
            QPushButton * button = (QPushButton *)ui->table_upload->cellWidget( i , 5 );
            //看按钮的状态 切换文字 发送信号
            if( button->text() == "暂停" ){
                //信号 设置文件信息结构体暂停标志位
                button->setText("开始");
                Q_EMIT SIG_setUploadPause( item0->m_info.timestamp , 1 );
            }
        }
    }
}


void MainDialog::slot_uploadResume(bool flag)
{
    qDebug() << "上传开始";
    int rows = ui->table_upload->rowCount();
    //遍历表单
    for( int i = 0 ; i < rows ; ++i ){
        MyTableWidgetItem * item0 = (MyTableWidgetItem *)ui->table_upload->item( i , 0 );
        //看是否打钩
        if( item0->checkState() == Qt::Checked ){
            QPushButton * button = (QPushButton *)ui->table_upload->cellWidget( i , 5 );
            //看按钮的状态 切换文字 发送信号
            if( button->text() == "开始" ){
                //信号 设置文件信息结构体暂停标志位
                button->setText("暂停");
                Q_EMIT SIG_setUploadPause( item0->m_info.timestamp , 0 );
            }
        }
    }
}

void MainDialog::slot_downloadPause(bool flag)
{
    qDebug() << " 下载暂停";
    int rows = ui->table_download->rowCount();
    //遍历表单
    for( int i = 0 ; i < rows ; ++i ){
        MyTableWidgetItem * item0 = (MyTableWidgetItem *)ui->table_download->item( i , 0 );
        //看是否打钩
        if( item0->checkState() == Qt::Checked ){
            QPushButton * button = (QPushButton *)ui->table_download->cellWidget( i , 5 );
            //看按钮的状态 切换文字 发送信号
            if( button->text() == "暂停" ){
                //信号 设置文件信息结构体暂停标志位
                button->setText("开始");
                Q_EMIT SIG_setDownloadPause( item0->m_info.timestamp , 1 );
            }
        }
    }
}

void MainDialog::slot_downloadResume(bool flag)
{
    qDebug() << "下载开始";
    int rows = ui->table_download->rowCount();
    //遍历表单
    for( int i = 0 ; i < rows ; ++i ){
        MyTableWidgetItem * item0 = (MyTableWidgetItem *)ui->table_download->item( i , 0 );
        //看是否打钩
        if( item0->checkState() == Qt::Checked ){
            QPushButton * button = (QPushButton *)ui->table_download->cellWidget( i , 5 );
            //看按钮的状态 切换文字 发送信号
            if( button->text() == "开始" ){
                //信号 设置文件信息结构体暂停标志位
                button->setText("暂停");
                Q_EMIT SIG_setDownloadPause( item0->m_info.timestamp , 0 );
            }
        }
    }
}

//批量设置表格中所有行的 暂停 / 开始 (不要求勾选)
void MainDialog::slot_setAllPause( QTableWidget * table , int isPause , bool isUpload )
{
    if( !table ) return;

    int rows = table->rowCount();
    for( int i = 0 ; i < rows ; ++i )
    {
        MyTableWidgetItem * item0 = (MyTableWidgetItem *)table->item( i , 0 );
        QPushButton * button = (QPushButton *)table->cellWidget( i , 5 );
        if( !item0 || !button ) continue;

        if( isPause && button->text() == "暂停" )
        {
            button->setText( "开始" );
            if( isUpload ) Q_EMIT SIG_setUploadPause( item0->m_info.timestamp , 1 );
            else           Q_EMIT SIG_setDownloadPause( item0->m_info.timestamp , 1 );
        }
        else if( !isPause && button->text() == "开始" )
        {
            button->setText( "暂停" );
            if( isUpload ) Q_EMIT SIG_setUploadPause( item0->m_info.timestamp , 0 );
            else           Q_EMIT SIG_setDownloadPause( item0->m_info.timestamp , 0 );
        }
    }
}

//插入到上传中
#include<QProgressBar>
//插入信息
void MainDialog::slot_insertUploadFile(FileInfo &info)
{
    //表格插入信息
    //列：文件名 大小 时间 速率 进度 按钮
    //1.新增一行 获取当前行+1 设置行数
    int rows = ui->table_upload->rowCount();
    ui->table_upload->setRowCount( rows +1);//设置行号
    //2.设置这一行的每一列控件( 添加对象 )

    MyTableWidgetItem *item0 = new MyTableWidgetItem;
    item0->slot_setInfo( info );

    ui->table_upload->setItem( rows , 0 , item0 );

    QTableWidgetItem * item1 = new QTableWidgetItem(
    FileInfo::getSize( info.size) );

    ui->table_upload->setItem( rows , 1 , item1 );

    QTableWidgetItem * item2 = new QTableWidgetItem( info.time );
    ui->table_upload->setItem( rows , 2 , item2 );

    QTableWidgetItem * item3 = new QTableWidgetItem( "0KB/s" );
    ui->table_upload->setItem( rows , 3 , item3 );

    //进度条
    QProgressBar* progress = new QProgressBar;
    progress->setMaximum( info.size );
    ui->table_upload->setCellWidget( rows , 4 , progress );

    //按钮
    QPushButton * button = new QPushButton;
    if( info.isPause == 0 ){
        button->setText( "暂停" );
    }else{
        button->setText( "开始" );
    }
    // 修复点：table_upload 在 ui 里只有5列(0~4)，原来的列号5越界，按钮显示不出来
    // 需要在 ui 中给 table_upload 增加一个"操作"列，这里放到第6列(index=5)
    ui->table_upload->setCellWidget( rows , 5 , button );

    //点击按钮 切换 暂停/开始
    connect( button , &QPushButton::clicked , this , [ this , button , item0 ](){
        if( button->text() == "暂停" )
        {
            button->setText( "开始" );
            Q_EMIT SIG_setUploadPause( item0->m_info.timestamp , 1 );
        }
        else
        {
            button->setText( "暂停" );
            Q_EMIT SIG_setUploadPause( item0->m_info.timestamp , 0 );
        }
    } );
}

void MainDialog::slot_insertDownloadFile(FileInfo &info)
{
    //表格插入信息
    //列：文件名 大小 时间 速率 进度 按钮
    //1.新增一行 获取当前行+1 设置行数
    int rows = ui->table_download->rowCount();
    ui->table_download->setRowCount( rows + 1 );

    //2.设置这一行的每一列控件( 添加对象 )
    MyTableWidgetItem *item0 = new MyTableWidgetItem;
    item0->slot_setInfo( info );
    ui->table_download->setItem( rows , 0 , item0 );

    QTableWidgetItem * item1 = new QTableWidgetItem(
    FileInfo::getSize(info.size) );
    ui->table_download->setItem( rows , 1 , item1 );

    QTableWidgetItem * item2 = new QTableWidgetItem( info.time );
    ui->table_download->setItem( rows , 2 , item2 );

    QTableWidgetItem * item3 = new QTableWidgetItem( "0KB/s" );
    ui->table_download->setItem( rows , 3 , item3 );

    //进度条
    QProgressBar* progress = new QProgressBar;
    progress->setMaximum( info.size );
    ui->table_download->setCellWidget( rows , 4 , progress );

    //按钮
    QPushButton * button = new QPushButton;
    if( info.isPause == 0 ){
        button->setText( "暂停" );
    }else{
        button->setText( "开始" );
    }
    ui->table_download->setCellWidget( rows , 5 , button );

    //点击按钮 切换 暂停/开始
    connect( button , &QPushButton::clicked , this , [ this , button , item0 ](){
        if( button->text() == "暂停" )
        {
            button->setText( "开始" );
            Q_EMIT SIG_setDownloadPause( item0->m_info.timestamp , 1 );
        }
        else
        {
            button->setText( "暂停" );
            Q_EMIT SIG_setDownloadPause( item0->m_info.timestamp , 0 );
        }
    } );
}

//插入上传完成
void MainDialog::slot_insertUploadComplete(FileInfo &info)
{
    //列：文件名 大小 时间 上传完成
    //1.新增一行 获取当前行+1 设置行数
    int rows = ui->table_complete->rowCount();//行
    ui->table_complete->setRowCount( rows +1);//设置行号
    //2.设置这一行的每一列控件( 添加对象 )

    MyTableWidgetItem *item0 = new MyTableWidgetItem;
    item0->slot_setInfo( info );

    ui->table_complete->setItem( rows , 0 , item0 );

    QTableWidgetItem * item1 = new QTableWidgetItem(
    FileInfo::getSize( info.size)  );
    ui->table_complete->setItem( rows , 1 , item1 );

    QTableWidgetItem * item2 = new QTableWidgetItem( info.time );
    ui->table_complete->setItem( rows , 2 , item2 );

    QTableWidgetItem * item3 = new QTableWidgetItem( "上传完成" );
    ui->table_complete->setItem( rows , 3 , item3 );
}

void MainDialog::slot_updateUploadFileProgress(int timestamp, int pos)
{
    //遍历所有项 第0列
    int row = ui->table_upload->rowCount();
    for( int i = 0 ; i < row ; ++i )
    {
        //取到每一个文件信息的时间戳 看是否一致
        MyTableWidgetItem * item0 = (MyTableWidgetItem *)ui->table_upload->item( i , 0 );
        if( item0->m_info.timestamp == timestamp )
        {
            //一致，更新进度
            QProgressBar * item4 = (QProgressBar *)ui->table_upload->cellWidget( i , 4 );
            item0->m_info.pos = pos;
            item4->setValue( pos );
            //看是否结束
            if( item4->value() >= item4->maximum() ){
                //是 删除这一项 添加到完成
                slot_insertUploadComplete( item0->m_info );
                slot_deleteUploadFileByRow( i );
                //return
                return;
            }

//            //看是否结束
//            if( item4->value() >= item4->maximum() )
//            {
//                // 修复点：原来一达到100%就立刻从"上传中"删除，小文件整轮上传在一帧内
//                // 完成，用户根本看不到记录。改成：先添加到"已完成"，停留800ms后再
//                // 从"上传中"删除，让用户能看到完成状态。
//                FileInfo completedInfo = item0->m_info;
//                slot_insertUploadComplete( completedInfo );

//                int ts = timestamp;
//                QTimer::singleShot(800, this, [this, ts](){
//                    int rows = ui->table_upload->rowCount();
//                    for( int j = 0; j < rows; ++j )
//                    {
//                        MyTableWidgetItem *it = (MyTableWidgetItem*)ui->table_upload->item(j, 0);
//                        if( it && it->m_info.timestamp == ts )
//                        {
//                            slot_deleteUploadFileByRow(j);
//                            break;
//                        }
//                    }
//                });
//                return;
//            }
        }
    }
}

void MainDialog::slot_updateDownloadFileProgress(int timestamp, int pos)
{
    //遍历所有项 第0列
    int row = ui->table_download->rowCount();
    for( int i = 0 ; i < row ; ++i )
    {
        //取到每一个文件信息的时间戳 看是否一致
        MyTableWidgetItem * item0 = (MyTableWidgetItem *)ui->table_download->item( i , 0 );
        if( item0->m_info.timestamp == timestamp )
        {
            //一致，更新进度
            QProgressBar * item4 = (QProgressBar *)ui->table_download->cellWidget(i,4);
            item0->m_info.pos = pos;
            item4->setValue( pos );

            //看是否结束
            if( item4->value() >= item4->maximum() )
            {
                //是 删除这一项 添加到完成

                slot_insertDownloadComplete( item0->m_info );
                slot_deleteDownloadFileByRow( i );
                return;
            }
        }
    }

}


void MainDialog::slot_deleteUploadFileByRow(int row)
{
    ui->table_upload->removeRow(row);
}

void MainDialog::slot_insertFileInfo(FileInfo &info)
{
    //列：文件名 大小 时间
    //1.新增一行 获取当前行+1 设置行数
    int rows = ui->table_file->rowCount() ;
    ui->table_file->setRowCount( rows + 1 );
    //2.设置这一行的每一列控件( 添加对象 )

    MyTableWidgetItem *item0 = new MyTableWidgetItem;
    item0->slot_setInfo( info );

    ui->table_file->setItem( rows , 0 , item0 );
    QString strSize ;
    if( info.type == "file" ){
        strSize = FileInfo::getSize (info.size);
    }else{
        strSize = " ";
    }

    QTableWidgetItem *item1 = new QTableWidgetItem( FileInfo::getSize( info.size) );

    ui->table_file->setItem( rows , 1 , item1 );

    QTableWidgetItem * item2 = new QTableWidgetItem( info.time );
    ui->table_file->setItem( rows , 2 , item2 );
}

void MainDialog::slot_deleteDownloadFileByRow(int row)
{
    ui->table_download->removeRow(row);
}
//下载完成的函数
void MainDialog::slot_insertDownloadComplete(FileInfo &info)
{
    //列：文件名 大小 时间 上传完成
    //1.新增一行 获取当前行+1 设置行数
    int rows = ui->table_complete->rowCount();
    ui->table_complete->setRowCount( rows + 1 );

    //2.设置这一行的每一列控件( 添加对象 )
    MyTableWidgetItem *item0 = new MyTableWidgetItem;
    item0->slot_setInfo( info );
    ui->table_complete->setItem( rows , 0 , item0 );

    QTableWidgetItem * item1 = new QTableWidgetItem( FileInfo::getSize(info.size) );
    ui->table_complete->setItem( rows , 1 , item1 );

    QTableWidgetItem * item2 = new QTableWidgetItem( info.time );
    ui->table_complete->setItem( rows , 2 , item2 );

    QPushButton * button = new QPushButton;

    button->setIcon( QIcon(":/images/folder.png") );
    //设置偏平
    button->setFlat( true );
    ///tooltip 提示
    button->setToolTip( info.absolutePath );

    connect( button , SIGNAL(clicked(bool)) , this , SLOT(slot_openPath(bool)) );

    ui->table_complete->setCellWidget(rows,3,button);
}

void MainDialog::slot_insertShareFileInfo(QString name, int size, QString time, int shareLink)
{
    //列：文件名 大小 时间 分享码
    //1.新增一行 获取当前行+1 设置行数
    int rows = ui->table_share->rowCount();
    ui->table_share->setRowCount( rows + 1 );
    //2.设置这一行的每一列控件（添加对象）

    QTableWidgetItem *item0 = new QTableWidgetItem( name );
    ui->table_share->setItem( rows , 0 , item0 );

    QTableWidgetItem * item1 = new QTableWidgetItem( FileInfo::getSize(size) );
    ui->table_share->setItem( rows , 1 , item1 );

    QTableWidgetItem * item2 = new QTableWidgetItem( time );
    ui->table_share->setItem( rows , 2 , item2 );

    QTableWidgetItem * item3 = new QTableWidgetItem( QString::number( shareLink ) );
    ui->table_share->setItem( rows , 3 , item3 );
}

void MainDialog::slot_openPath( bool flag )
{
    QPushButton * button = (QPushButton *)QObject::sender();
    QString path = button->toolTip();

    // 转化 \\
    path.replace( '/' , '\\' );
    qDebug() << path;

    // 使用 QDesktopServices 打开文件所在文件夹，比 explorer /select 更稳定
    QFileInfo fileInfo(path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(fileInfo.absolutePath()));
}

void MainDialog::slot_deleteAllFileInfo()
{
    //ui->table_file->clear(); //删文字 不用这个 行数不变

    int rows = ui->table_file->rowCount();
    for( int i = rows - 1 ; i >= 0 ; i-- )
    {
        ui->table_file->removeRow(i);
    }
}

void MainDialog::slot_deleteAllShareInfo()
{
    int rows = ui->table_share->rowCount();
    for( int i = rows - 1 ; i >= 0 ; i-- )
    {
        ui->table_share->removeRow(i);
    }
}



//选中某一行
//选中某一行
void MainDialog::on_table_file_cellClicked(int row, int column)
{
    //切换 勾选和未勾选状态
    MyTableWidgetItem * item0 = (MyTableWidgetItem *)ui->table_file->item( row , 0 );
    if( item0->checkState() == Qt::Checked ){
        item0->setCheckState( Qt::Unchecked );
    }else{
        item0->setCheckState( Qt::Checked );
    }
}


//表格位置鼠标右键
void MainDialog::on_table_file_customContextMenuRequested(const QPoint &pos)
{
    //弹出菜单
    m_menuFileInfo.exec( QCursor::pos() );
}

//双击点击文件夹：
void MainDialog::on_table_file_cellDoubleClicked(int row, int column)
{
    //首先 拿到双击的那行的文件名字
    MyTableWidgetItem * item0 = (MyTableWidgetItem *)ui->table_file->item( row, 0 );

    // 判断是不是文件夹 是文件夹 可以跳转( 是文件 考虑 打开文件 todo )
    if( item0->m_info.type != "file" )
    {
        // 是文件夹 路径 拼接
        QString dir = ui->lb_path->text() + item0->m_info.name + "/";
        //设置路径 lb_path ->text
        ui->lb_path->setText( dir );
        //发送信号 -> 更新当前的目录 -> 刷新文件列表
        Q_EMIT SIG_changeDir( dir );
    }
}


void MainDialog::on_pb_prew_clicked()
{
    //获取目录
        QString path = ui->lb_path->text();
        //判断根目录 "/"，根目录无法继续返回上级
        if( path == "/" ) return;

        // 首先找到最右边的 "/"，截取到该位置，去掉末尾文件夹名
        path = path.left( path.lastIndexOf( "/" ) );

        //再次截取，保留到上一级的 "/"，构成上级目录路径
        path = path.left( path.lastIndexOf( "/" ) + 1 );
        qDebug() << path ;
        ui->lb_path->setText(path);
        //发射切换目录信号，跳转到上级目录
        Q_EMIT SIG_changeDir( path );
}


void MainDialog::on_table_upload_cellClicked(int row, int column)
{
    //切换 勾选和未勾选状态
    MyTableWidgetItem * item0 = (MyTableWidgetItem *)ui->table_upload->item( row , 0 );
    if( item0->checkState() == Qt::Checked ){
        item0->setCheckState( Qt::Unchecked );
    }else{
        item0->setCheckState( Qt::Checked );
    }
}


void MainDialog::on_table_download_cellClicked(int row, int column)
{
    //切换 勾选和未勾选状态
    MyTableWidgetItem * item0 = (MyTableWidgetItem *)ui->table_download->item( row , 0 );
    if( item0->checkState() == Qt::Checked ){
        item0->setCheckState( Qt::Unchecked );
    }else{
        item0->setCheckState( Qt::Checked );
    }
}

bool MainDialog::slot_getDownloadFileInfoByTimestamp(int timestamp,FileInfo &info)
{
    //遍历所有第0列
    int rows = ui->table_download->rowCount();

    for( int i = 0 ; i < rows ; ++i ){
        MyTableWidgetItem * item0 = (MyTableWidgetItem *)ui->table_download->item( i , 0);
        if( item0->m_info.timestamp == timestamp ){
            info= item0->m_info;
            return true;
        }
        return false;
    }
}

bool MainDialog::slot_getUploadFileInfoByTimestamp(int timestamp,FileInfo & info)
{
    //遍历所有第0列
        int rows = ui->table_upload->rowCount();

        for( int i = 0 ; i < rows ; ++i ){
            MyTableWidgetItem * item0 = (MyTableWidgetItem *)ui->table_upload->item( i , 0);
            if( item0->m_info.timestamp == timestamp ){
                info= item0->m_info;
                return true;
            }
        }
        return false;
}


