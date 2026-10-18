#ifndef VIDEOPLAYER_H
#define VIDEOPLAYER_H

#include<QImage>
#include <QThread>
#include<PacketQueue.h>

#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000 //1 second of 48khz 32bit audio
#define SDL_AUDIO_BUFFER_SIZE 1024 //

//跳转时往队列中放入的特殊包, 解码线程取到该包后清空解码器缓存(避免花屏)
#define FLUSH_DATA "FLUSH"

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
#include "libavdevice/avdevice.h"
#include "libavutil/time.h"
#include "SDL.h"
}

class VideoPlayer ;

//播放状态
enum PlayerState
{
    Playing = 0,
    Pause,
    Stop
};

typedef struct VideoState {
    AVFormatContext *pFormatCtx;//相当于视频”文件指针”
    ///////////////音频///////////////////////////////////
    AVStream *audio_st;             //音频流
    PacketQueue *audioq;            //音频缓冲队列
    AVCodecContext *pAudioCodecCtx ;//音频解码器信息指针
    int audioStream;                //音频流索引
    double audio_clock;             ///<pts of last decoded frame 音频时钟(单位:微秒)
    SDL_AudioDeviceID audioID;      //音频设备 ID
    SDL_AudioFormat audio_fmt;      //音频设备实际格式(供 SDL_MixAudioFormat 使用)
    AVFrame out_frame;              //设置参数，供音频解码后的 swr_alloc_set_opts 使用。
    /// 音频回调函数使用的量
    uint8_t audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
    unsigned int audio_buf_size;
    unsigned int audio_buf_index;
    AVFrame *audioFrame;
    //////////////////////////////////////////////////////
    ///////////////视频///////////////////////////////////
    AVStream *video_st;         //视频流
    PacketQueue *videoq;        //视频队列
    AVCodecContext *pCodecCtx ; //视频解码器信息指针
    int videoStream;            //视频流索引
    double video_clock;         ///<pts of last decoded frame 视频时钟(单位:微秒)
    SDL_Thread *video_tid;      //视频线程 id
    /////////////////////////////////////////////////////

    //////////播放控制的变量////////////////////////////////
    int isPause;                //暂停标志
    int quit;                   //1 表示请求退出
    int readFinished;           //读线程文件读取完毕
    int readThreadFinished;     //读取线程是否结束
    int videoThreadFinished;    //视频线程是否结束
    /////////////////////////////////////////////////////

    //////////跳转相关的变量///////////////////////////////
    int seek_req;               //跳转标志 --读线程
    int64_t seek_pos;           //跳转的位置 --微秒
    int seek_flag_audio;        //跳转标志 --用于音频线程中
    int seek_flag_video;        //跳转标志 --用于视频线程中
    double seek_time;           //跳转的时间(秒) 值和seek_pos是一样的(微秒)
    /////////////////////////////////////////////////////

    int volume;                 //音量 0~SDL_MIX_MAXVOLUME(128)
    int64_t start_time;         //单位 微秒
    VideoPlayer* m_player;      //用于调用函数(抛信号给界面)

    //注意: 所有的指针/索引必须在这里初始化, 否则 run() 里使用到的是野值
    VideoState()
        : pFormatCtx(NULL)
        , audio_st(NULL)
        , audioq(NULL)
        , pAudioCodecCtx(NULL)
        , audioStream(-1)
        , audio_clock(0)
        , audioID(0)
        , audio_fmt(AUDIO_S16SYS)
        , audio_buf_size(0)
        , audio_buf_index(0)
        , audioFrame(NULL)
        , video_st(NULL)
        , videoq(NULL)
        , pCodecCtx(NULL)
        , videoStream(-1)
        , video_clock(0)
        , video_tid(NULL)
        , isPause(0)
        , quit(0)
        , readFinished(0)
        , readThreadFinished(1) //初始状态读线程是"已结束", 避免外部 stop 等待卡死
        , videoThreadFinished(1)
        , seek_req(0)
        , seek_pos(0)
        , seek_flag_audio(0)
        , seek_flag_video(0)
        , seek_time(0)
        , volume(SDL_MIX_MAXVOLUME)
        , start_time(0)
        , m_player(NULL)
    {
        memset( &out_frame , 0 , sizeof(out_frame) );
        memset( audio_buf , 0 , sizeof(audio_buf) );
    }
} VideoState;

//网络打开用的"打断/超时"上下文
//背景: RTMP/HLS 底层是 TCP, 服务器不可达时 connect 会一直阻塞(界面直接卡死)
//解决: 给 AVFormatContext 设一个 interrupt_callback, ffmpeg 在阻塞期间会反复回调它,
//      回调返回 1 就打断阻塞, avformat_open_input 随即返回错误码, 走正常的失败分支
typedef struct NetOpenCtx {
    int64_t startUsec;      //开始打开的时间(微秒)
    int connected;          //1: 已经打开成功(之后正常读数据, 不再打断)
    volatile int *quit;     //播放器停止标志(点了停止立刻打断阻塞, 避免退出卡住)
} NetOpenCtx;

class VideoPlayer : public QThread
{
    Q_OBJECT
    //qt通过信号槽 实现qt 多线程通信

signals:
    void SIG_getOneImage( QImage img );             //解码出一帧图像
    void SIG_PlayerStateChanged( int state );       //播放状态改变(Stop/Playing/Pause)
    void SIG_TotalTime( qint64 uSec );              //视频总时长(微秒)

public:
    explicit VideoPlayer(QObject *parent = 0);
    ~VideoPlayer();

    void run();

    //////////////////播放控制/////////////////////////
    void setFileName(const QString &fileName);  //设置文件名并开始播放
    void play();                                //恢复播放(暂停->播放)
    void pause();                               //暂停
    void stop(bool isWait = true);              //停止播放, isWait 是否等待读取线程退出
    void seek(int64_t pos);                     //跳转, pos 精确到微秒
    double getCurrentTime();                    //当前播放时间(微秒)
    int64_t getTotalTime();                     //视频总时间(微秒)
    PlayerState getPlayerState() const;         //当前播放状态
    void setVolume(int volume);                 //音量 0~128

    /// 当前播放的文件名
    const QString& fileName() const { return m_filename; }

    /// 供视频解码线程(SDL线程)调用, 抛信号给界面显示
    void SendGetOneImage(QImage img);

private:
    QString m_filename;

    VideoState m_videoState;
    PlayerState m_playerState;

};

#endif // VIDEOPLAYER_H
