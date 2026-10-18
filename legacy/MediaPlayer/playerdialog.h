#ifndef PLAYERDIALOG_H
#define PLAYERDIALOG_H
#include"videoplayer.h"

#include <QDialog>
#include <QTimer>
#include <QElapsedTimer>

QT_BEGIN_NAMESPACE
namespace Ui { class PlayerDialog; }
QT_END_NAMESPACE



class PlayerDialog : public QDialog
{
    Q_OBJECT

public:
    PlayerDialog(QWidget *parent = nullptr);
    ~PlayerDialog();

    //打开文件/网络流并立即播放(供命令行自动播放/测试使用)
    //path 可以是本地文件路径, 也可以是 rtmp:// http:// 等网络地址
    void openAndPlay(const QString &path);

    //打开网络流(直播 rtmp / 点播 rtmp / HLS http), 不检查本地文件是否存在
    void openUrl(const QString &url);

private slots:
    void on_pb_open_clicked();   //打开文件并播放
    void on_pb_openUrl_clicked();//打开网络流(地址栏里填的 rtmp/hls 地址)
    void on_pb_play_clicked();   //恢复播放
    void on_pb_pause_clicked();  //暂停
    void on_pb_stop_clicked();   //停止

    void slot_setImage(QImage img);                 //显示解码出的一帧
    void slot_PlayerStateChanged(int state);        //播放状态切换
    void slot_getTotalTime(qint64 uSec);            //获取视频总时长
    void slot_TimerTimeOut();                       //定时器刷新当前播放时间
    void slot_videoSliderValueChanged(int value);   //手动点击进度条跳转

private:
    Ui::PlayerDialog *ui;

    VideoPlayer*m_player;
    QTimer *m_Timer;    //定时器-获取当前视频时间
    bool isStop;        //代表当前是否是停止状态

    //跳转节流: 网络流(HLS)频繁拖动会瞬间发出大量请求, 太密集的直接忽略
    QElapsedTimer m_lastSeek;
};
#endif // PLAYERDIALOG_H
