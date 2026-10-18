#include "playerdialog.h"
#include "ui_playerdialog.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QDebug>

#define _DEF_PATH "D:/"

//判断是不是网络流地址(本地文件走原来的"是否存在"检查, 网络地址不需要)
static bool isNetworkUrl(const QString &s)
{
    return s.startsWith("rtmp://", Qt::CaseInsensitive)
        || s.startsWith("rtsp://", Qt::CaseInsensitive)
        || s.startsWith("http://", Qt::CaseInsensitive)
        || s.startsWith("https://", Qt::CaseInsensitive)
        || s.startsWith("udp://", Qt::CaseInsensitive)
        || s.startsWith("tcp://", Qt::CaseInsensitive);
}

PlayerDialog::PlayerDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::PlayerDialog)
    , m_player(NULL)
    , m_Timer(NULL)
    , isStop(true)
{
    ui->setupUi(this);
    m_player = new VideoPlayer( this );
    //信号和槽链接
    connect( m_player , SIGNAL(SIG_getOneImage(QImage)) , this , SLOT( slot_setImage(QImage) ) );
    connect( m_player , SIGNAL(SIG_PlayerStateChanged(int)) , this , SLOT( slot_PlayerStateChanged(int) ) );
    connect( m_player , SIGNAL(SIG_TotalTime(qint64)) , this , SLOT( slot_getTotalTime(qint64) ) );
    //视频进度通过手动点击改变
    connect( ui->slider_videoProcess , SIGNAL(SIG_valueChanged(int)) , this ,
             SLOT( slot_videoSliderValueChanged(int) ) );
    //音量
    connect( ui->slider_volume , &QSlider::valueChanged , m_player , &VideoPlayer::setVolume );

    m_Timer = new QTimer; //定时器-获取当前视频时间
    connect( m_Timer , SIGNAL(timeout()) , this , SLOT( slot_TimerTimeOut() ) );
    m_Timer->setInterval(500);

    //初始状态: 停止
    slot_PlayerStateChanged( PlayerState::Stop );
}

PlayerDialog::~PlayerDialog()
{
    //销毁前先让播放线程退出, 否则线程还在跑就 delete 会崩溃
    if( m_player ) m_player->stop(true);
    if( m_Timer ) m_Timer->stop();
    delete m_player;
    delete ui;
}

//打开文件/网络流并立即播放(供命令行自动播放/测试使用)
//用法: MediaPlayer.exe D:/xxx.mp4   或   MediaPlayer.exe rtmp://192.168.142.128:1935/videotest
void PlayerDialog::openAndPlay(const QString &path)
{
    if( isNetworkUrl( path ) )
    {
        openUrl( path );
        return;
    }

    QFileInfo info( path );
    if( !info.exists() )
    {
        qDebug() << "file not exists:" << path;
        return;
    }
    m_player->stop( true ); //如果播放你要先关闭
    m_player->setFileName( path );
    ui->lb_videoName->setText( info.fileName() );
    setWindowTitle( QString("视频播放器 - %1").arg( info.fileName() ) );
    slot_PlayerStateChanged( PlayerState::Playing );
}

//打开网络流: 直播(rtmp://ip/videotest) / 点播(rtmp://ip/vod/1.flv) / HLS(http://ip/hls/xx/playlist.m3u8)
void PlayerDialog::openUrl(const QString &url)
{
    if( url.isEmpty() ) return;

    m_player->stop( true );      //正在播的先停掉
    m_player->setFileName( url );//直接把网络地址交给 ffmpeg 打开
    ui->le_url->setText( url );
    ui->lb_videoName->setText( url );
    setWindowTitle( QString("视频播放器 - %1").arg( url ) );
    slot_PlayerStateChanged( PlayerState::Playing );
}

//地址栏 -> 打开网络流
void PlayerDialog::on_pb_openUrl_clicked()
{
    QString url = ui->le_url->text().trimmed();
    //粘贴地址时经常会带上空格, 这里去掉(VLC/ffmpeg 遇到尾部空格会打不开)
    url.remove(' ');
    if( url.isEmpty() ) return;

    if( !isNetworkUrl( url ) )
    {
        QMessageBox::information( this , "提示" ,
            "请输入完整的网络地址, 例如:\n"
            "rtmp://192.168.142.128:1935/videotest\n"
            "http://192.168.142.128/hls/xxx/playlist.m3u8" );
        return;
    }
    openUrl( url );
}

//播放状态切换槽函数
void PlayerDialog::slot_PlayerStateChanged(int state)
{
    switch( state )
    {
    case PlayerState::Stop:
        qDebug() << "VideoPlayer::Stop";
        if( m_Timer ) m_Timer->stop();
        ui->slider_videoProcess->setValue(0);
        ui->lb_totalTime->setText("00:00:00");
        ui->lb_currentTime->setText("00:00:00");
        ui->lb_videoName->setText("");
        ui->pb_play->show();
        ui->pb_pause->hide();
        {
            QImage img;
            img.fill( Qt::black );
            slot_setImage( img );
        }
        this->update();
        isStop = true;
        break;
    case PlayerState::Playing:
        qDebug() << "VideoPlayer::Playing";
        ui->pb_play->hide();
        ui->pb_pause->show();
        if( m_Timer ) m_Timer->start();
        this->update();
        isStop = false;
        break;
    default:
        break;
    }
}

//打开文件
void PlayerDialog::on_pb_open_clicked()
{
    //打开文件弹出对话框参数:父窗口, 标题, 默认路径, 筛选器
    QString path = QFileDialog::getOpenFileName( this , "选择要播放的文件" , _DEF_PATH ,
            "视频文件 (*.flv *.rmvb *.avi *.mp4 *.mkv *.mov);;音频文件 (*.mp3 *.wav *.aac);;所有文件 (*.*)" );
    if( !path.isEmpty() )
    {
        qDebug() << path ;
        QFileInfo info( path );
        if( info.exists() )
        {
            m_player->stop( true ); //如果播放你要先关闭
            m_player->setFileName( path );
            ui->lb_videoName->setText( info.fileName() );
            slot_PlayerStateChanged( PlayerState::Playing );
        }
        else
        {
            QMessageBox::information( this , "提示" , "打开文件失败" );
        }
    }
}

//恢复播放
void PlayerDialog::on_pb_play_clicked()
{
    if( isStop ) return; //停止状态自然不能播放
    m_player->play();
    if( m_player->getPlayerState() == PlayerState::Playing )
    {
        ui->pb_play->hide();
        ui->pb_pause->show();
        this->update();
    }
}

//暂停
void PlayerDialog::on_pb_pause_clicked()
{
    if( isStop ) return; //停止状态自然不能暂停
    m_player->pause();
    if( m_player->getPlayerState() == PlayerState::Pause )
    {
        ui->pb_play->show();
        ui->pb_pause->hide();
        this->update();
    }
}

//停止
void PlayerDialog::on_pb_stop_clicked()
{
    m_player->stop( true );
}

void PlayerDialog::slot_setImage(QImage img)
{
    //交给 OpenGL 控件渲染: 由显卡做缩放, 不再用 Qt 的 scaled(画质更好, 也更省CPU)
    //img 为空表示停止/播放结束 -> 清屏显示黑屏
    ui->wdg_glShow->setImage( img );
}

//获取视频全部时间
void PlayerDialog::slot_getTotalTime(qint64 uSec)
{
    qint64 Sec = uSec / 1000000;
    ui->slider_videoProcess->setRange( 0 , Sec );//精确到秒

    QString hStr = QString("00%1").arg(Sec/3600);
    QString mStr = QString("00%1").arg(Sec/60);
    QString sStr = QString("00%1").arg(Sec%60);
    QString str = QString("%1:%2:%3").arg(hStr.right(2)).arg(mStr.right(2)).arg(sStr.right(2));
    ui->lb_totalTime->setText( str );
}

//获取当前视频时间定时器
void PlayerDialog::slot_TimerTimeOut()
{
    if ( QObject::sender() != m_Timer ) return;

    qint64 Sec = m_player->getCurrentTime() / 1000000;

    if( !ui->slider_videoProcess->isSliderDown() ) //用户拖动时不覆盖
        ui->slider_videoProcess->setValue( Sec );

    QString hStr = QString("00%1").arg(Sec/3600);
    QString mStr = QString("00%1").arg(Sec/60%60);
    QString sStr = QString("00%1").arg(Sec%60);
    QString str = QString("%1:%2:%3").arg(hStr.right(2)).arg(mStr.right(2)).arg(sStr.right(2));
    ui->lb_currentTime->setText( str );

    //播放到结尾: 进度条到最大值 且 播放器已停止
    if( ( ui->slider_videoProcess->value() >= ui->slider_videoProcess->maximum() - 1 )
            && m_player->getPlayerState() == PlayerState::Stop )
    {
        slot_PlayerStateChanged( PlayerState::Stop );
    }
}

//视频进度通过手动点击改变
void PlayerDialog::slot_videoSliderValueChanged(int value)
{
    if( QObject::sender() == ui->slider_videoProcess )
    {
        if( m_player->getPlayerState() == PlayerState::Stop ) return; //没在播放不能跳转

        //节流: 300ms 内只响应一次(网络流连续拖动会瞬间发出大量请求, 服务端扛不住)
        if( m_lastSeek.isValid() && m_lastSeek.elapsed() < 300 ) return;
        m_lastSeek.restart();

        m_player->seek( (qint64)value * 1000000 );  //value 秒 -> 微秒
    }
}
