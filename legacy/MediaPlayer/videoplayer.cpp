#include "videoplayer.h"
#include <QDebug>
#include <QCoreApplication>
#include <string>
#include <assert.h>

//当队列里面的数据超过某个大小的时候 就暂停读取 防止一下子就把视频读完了，导致的空间分配不足
#define MAX_AUDIO_SIZE  (1024*16*25*10) //音频阈值
#define MAX_VIDEO_SIZE  (1024*255*25*2) //视频阈值

//时间补偿函数--视频延时
static double synchronize_video(VideoState *is, AVFrame *src_frame, double pts);
//视频解码线程函数(SDL线程)
static int video_thread(void *arg);
//音频回调函数(SDL 音频线程)
static void audio_callback(void *userdata, Uint8 *stream, int len);
//解码函数
static int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size);
//找 audio_stream / video_stream
static int find_stream_index(AVFormatContext *pformat_ctx, int *video_stream, int *audio_stream);
//往队列里放入 FLUSH_DATA 特殊包(跳转时用于清空解码器)
static AVPacket* alloc_flush_packet();
//网络超时回调: 返回 1 表示打断 ffmpeg 当前的阻塞操作
static int interrupt_open_cb(void *ctx);

VideoPlayer::VideoPlayer(QObject *parent)
    : QThread(parent)
    , m_playerState( PlayerState::Stop )
{
}

VideoPlayer::~VideoPlayer()
{
    stop(true);
}

//////////////////播放控制/////////////////////////

//设置文件名并开始播放
void VideoPlayer::setFileName(const QString &fileName)
{
    if( m_playerState != PlayerState::Stop ) return; //播放中不允许重复打开
    m_filename = fileName;
    m_videoState.quit = 0; //清除上一次的停止请求(stop 置位的)
    m_playerState = PlayerState::Playing;
    this->start();
}

//恢复播放(暂停->播放)
void VideoPlayer::play()
{
    m_videoState.isPause = 0;
    if( m_playerState != PlayerState::Pause ) return;
    m_playerState = PlayerState::Playing;
}

//暂停
void VideoPlayer::pause()
{
    m_videoState.isPause = 1;
    if( m_playerState != PlayerState::Playing ) return;
    m_playerState = PlayerState::Pause;
}

//跳转, pos 精确到微秒
void VideoPlayer::seek(int64_t pos)
{
    if( !m_videoState.seek_req )
    {
        m_videoState.seek_pos = pos;
        m_videoState.seek_req = 1;
    }
}

//获取当前时间(微秒)
double VideoPlayer::getCurrentTime()
{
    //纯视频文件没有音频时钟, 用视频时钟
    if( m_videoState.audioStream != -1 )
        return m_videoState.audio_clock;
    return m_videoState.video_clock;
}

//获取总时间(微秒)
int64_t VideoPlayer::getTotalTime()
{
    if( m_videoState.pFormatCtx )
        return m_videoState.pFormatCtx->duration;
    return -1;
}

PlayerState VideoPlayer::getPlayerState() const
{
    return m_playerState;
}

//音量 0~128
void VideoPlayer::setVolume(int volume)
{
    if( volume < 0 ) volume = 0;
    if( volume > SDL_MIX_MAXVOLUME ) volume = SDL_MIX_MAXVOLUME;
    m_videoState.volume = volume;
}

//播放停止, 用于外面调用
void VideoPlayer::stop(bool isWait)
{
    m_videoState.quit = 1;
    //唤醒阻塞在队列上的解码线程, 让它们能退出
    if( m_videoState.videoq ) packet_queue_abort( m_videoState.videoq );
    if( m_videoState.audioq ) packet_queue_abort( m_videoState.audioq );

    if( isWait ) //阻塞标志
    {
        while( !m_videoState.readThreadFinished ) //等待读取线程退出
        {
            SDL_Delay(10);
        }
    }
    //确保 QThread 本身也结束, 否则紧接着 start() 会无效
    if( isRunning() )
    {
        wait( 3000 );
    }
    m_playerState = PlayerState::Stop;
    Q_EMIT SIG_PlayerStateChanged( PlayerState::Stop );
}

//发送图片信号函数
void VideoPlayer::SendGetOneImage(QImage img)
{
    emit SIG_getOneImage(img); //发送信号
}

//网络超时回调(ffmpeg 在 connect/读数据阻塞时会反复调用)
//返回 1 -> 打断阻塞; 返回 0 -> 继续等
static int interrupt_open_cb(void *ctx)
{
    NetOpenCtx *c = (NetOpenCtx *)ctx;
    if( !c ) return 0;

    //1) 用户点了停止 -> 立刻打断, 保证退出不卡
    if( c->quit && *(c->quit) ) return 1;

    //2) 已经连上了 -> 正常读数据, 不要打断
    if( c->connected ) return 0;

    //3) 连了 5 秒还没连上(比如填了个不存在的 IP) -> 判定超时
    if( av_gettime() - c->startUsec > 5 * 1000000 ) return 1;

    return 0;
}

//往队列里放入 FLUSH_DATA 特殊包
static AVPacket* alloc_flush_packet()
{
    AVPacket *packet = (AVPacket *) av_malloc(sizeof(AVPacket)); //分配一个 packet
    av_init_packet(packet);
    av_new_packet(packet, 10);
    strcpy((char*)packet->data, FLUSH_DATA);
    return packet;
}

//时间补偿函数--视频延时
static double synchronize_video(VideoState *is, AVFrame *src_frame, double pts)
{
    double frame_delay; // 缓存帧和帧之间的延迟

    if (pts != 0) {
        /* if we have pts, set video clock to it */
        // 如果当前帧有 PTS 时间戳，那么使用它来更新视频时钟
        is->video_clock = pts;
    } else {
        /* if we aren't given a pts, set it to the clock */
        // 如果没有 PTS 时间戳，则采用视频时钟作为当前时间
        pts = is->video_clock;
    }
    /* update the video clock */
    // 计算当前帧和上一帧之间的时钟差(用流的 time_base, pts 就是以它为单位的)
    frame_delay = av_q2d(is->video_st->time_base);
    /* if we are repeating a frame, adjust clock accordingly */
    // 如果当前帧是重复帧，需要根据重复数调整帧之间的时间差
    frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
    // 更新视频时钟(换算成微秒)
    is->video_clock += frame_delay * 1000000.0;
    // 返回当前帧的 PTS 时间戳
    return pts;
}

//视频解码线程函数
static int video_thread(void *arg)
{
    VideoState *is = (VideoState *) arg;
    AVPacket pkt1, *packet = &pkt1;
    int ret, got_picture, numBytes;
    double video_pts = 0; //当前视频的 pts(微秒)
    double audio_pts = 0; //音频 pts(微秒)
    ///解码视频相关
    AVFrame *pFrame, *pFrameRGB;
    uint8_t *out_buffer_rgb; //解码后的 rgb 数据
    struct SwsContext *img_convert_ctx; //用于解码后的视频格式转换

    AVCodecContext *pCodecCtx = is->pCodecCtx; //视频解码器

    pFrame = av_frame_alloc();
    pFrameRGB = av_frame_alloc();

    ///这里我们改成了 将解码后的 YUV 数据转换成 RGB32
    img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
                                     pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
                                     AV_PIX_FMT_RGB32, SWS_BICUBIC, NULL, NULL, NULL);

    numBytes = avpicture_get_size(AV_PIX_FMT_RGB32, pCodecCtx->width, pCodecCtx->height);
    out_buffer_rgb = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
    avpicture_fill((AVPicture *) pFrameRGB, out_buffer_rgb, AV_PIX_FMT_RGB32,
                   pCodecCtx->width, pCodecCtx->height);

    while( !is->quit )
    {
        if( is->isPause ) //暂停
        {
            SDL_Delay(10);
            continue;
        }

        //从队列中读取数据: 首先不能阻塞, 不然无法退出
        //有读取慢, 解析快的情况(播放网络) 队列里没东西不能退出
        //音频没有解析完也不能退出
        if (packet_queue_get(is->videoq, packet, 0) <= 0)
        {
            //播放到结束
            if( is->readFinished && ( !is->audioq || is->audioq->nb_packets == 0 ) )
            {//读线程完毕
                break;
            }
            else
            {
                SDL_Delay(1);   //只是队列里面暂时没有数据而已
                continue;
            }
        }

        //收到 FLUSH_DATA 说明刚刚执行过跳转, 现在需要把解码器的数据清除一下
        if( strcmp((char*)packet->data, FLUSH_DATA) == 0 )
        {
            avcodec_flush_buffers( is->pCodecCtx );
            av_packet_unref( packet );
            is->video_clock = 0; //很关键, 不清空 向左跳转, 视频帧会一直等待音频帧
            continue;
        }

        //音视频同步: 以音频时钟为主时钟, 视频快了就延时
        if( is->audioStream != -1 )
        {
            int wait_count = 0;
            while( !is->quit )
            {
                audio_pts = is->audio_clock;
                if (video_pts <= audio_pts) break;
                if( is->isPause ) break; //暂停时不等待, 由暂停统一控制
                if (++wait_count > 100) break; //最多等 500ms, 防止音频时钟异常时卡死
                SDL_Delay(5);
            }
        }
        else
        {
            //没有音频流: 用系统时钟做同步, 保证按正常速度播放
            if( is->start_time == 0 )
                is->start_time = av_gettime() - (int64_t)video_pts;
            int64_t now = av_gettime() - is->start_time;
            int wait_count = 0;
            while( !is->quit && video_pts > (double)now )
            {
                SDL_Delay(5);
                now = av_gettime() - is->start_time;
                if( ++wait_count > 2000 ) break;
            }
        }

        got_picture = 0;
        ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, packet);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error decoding video frame\n");
            av_packet_unref(packet); //新版使用 av_packet_unref() 代替 av_free_packet()
            continue;
        }

        //获取显示时间 pts(换算成微秒)
        int64_t best_pts = pFrame->best_effort_timestamp;
        if( best_pts == AV_NOPTS_VALUE ) best_pts = 0;
        video_pts = (double)best_pts * av_q2d(is->video_st->time_base) * 1000000.0;
        video_pts = synchronize_video(is, pFrame, video_pts);//视频时钟补偿

        //发生了跳转 则跳过关键帧到目的时间的这几帧
        if( is->seek_flag_video )
        {
            if( video_pts < is->seek_time )
            {
                av_packet_unref(packet);
                continue;
            }
            else
            {
                is->seek_flag_video = 0;
            }
        }

        if (got_picture) {
            sws_scale(img_convert_ctx,
                      (uint8_t const * const *) pFrame->data,
                      pFrame->linesize, 0, pCodecCtx->height, pFrameRGB->data,
                      pFrameRGB->linesize);
            //把这个 RGB 数据 用 QImage 加载
            QImage tmpImg((uchar *)out_buffer_rgb, pCodecCtx->width, pCodecCtx->height,
                          QImage::Format_RGB32);
            QImage image = tmpImg.copy(); //把图像复制一份 传递给界面显示
            is->m_player->SendGetOneImage(image); //调用激发信号的函数
        }

        av_packet_unref(packet); //新版使用 av_packet_unref() 来代替 av_free_packet()
    }

    //在退出函数之前, 设置标志位 -->控制读取文件线程音频线程的结束
    if( !is->quit )
    {
        is->quit = 1;
    }
    sws_freeContext( img_convert_ctx );
    av_frame_free( &pFrame );
    av_frame_free( &pFrameRGB );
    av_free( out_buffer_rgb );
    is->videoThreadFinished = 1;

    //清屏: 播放结束/停止后把界面刷成黑色
    QImage img( 16, 16, QImage::Format_RGB32 );
    img.fill(Qt::black);
    if( is->m_player ) is->m_player->SendGetOneImage(img); //调用激发信号的函数
    return 0;
}

void VideoPlayer::run()
{
    qDebug() << "VideoPlayer::" << __func__ ;

    //如果上一轮停止请求还没来得及处理, 直接退出(防止新状态把 quit 冲掉)
    if( m_videoState.quit )
    {
        m_playerState = PlayerState::Stop;
        return;
    }

    //音频
    //添加音频需要的变量
    int audioStream = -1;//音频解码器需要的流的索引
    AVCodecContext *pAudioCodecCtx = NULL;//音频解码器信息指针
    AVCodec *pAudioCodec = NULL; //音频解码器
    //SDL
    SDL_AudioSpec wanted_spec; //SDL 音频设置
    SDL_AudioSpec spec ; //SDL 音频设置
    memset( &wanted_spec , 0 , sizeof(wanted_spec) );
    memset( &spec , 0 , sizeof(spec) );

    //视频
    int videoStream = -1;
    AVCodecContext *pCodecCtx = NULL;  //视频的解码器信息指针
    AVCodec *pCodec = NULL;            //视频解码器
    AVPacket *packet = NULL;           //读取解码前的包

    //1. 初始化
    //1.初始化 FFmpeg 调用了这个才能正常适用编码器和解码器 注册所有函数
    av_register_all();
    //支持网络协议(rtmp / http / hls / rtsp ...), 不加这个打不开网络流
    avformat_network_init();
    //SDL 初始化
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
    {
        qDebug()<< "Couldn't init SDL: " << SDL_GetError();
        m_playerState = PlayerState::Stop;
        return;
    }
    //每次播放都重置状态(注意: 不能用 memset 一个含有非 POD 成员的结构体)
    m_videoState = VideoState();

    //2. 分配 AVFormatContext
    //FFMPEG 所有操作都要通过 AVFormatContext 来进行, 可以理解为视频文件指针
    AVFormatContext *pFormatCtx = avformat_alloc_context();

    //2.1 设置网络超时回调(本地文件也不影响: 打开很快, 不会触发超时)
    NetOpenCtx netCtx;
    netCtx.startUsec = av_gettime();
    netCtx.connected = 0;
    netCtx.quit = &m_videoState.quit;
    pFormatCtx->interrupt_callback.callback = interrupt_open_cb;
    pFormatCtx->interrupt_callback.opaque   = &netCtx;

    //3. 打开视频文件并获取信息
    //中文兼容：FFmpeg 在 Windows 下要求 UTF-8 路径，toStdString() 是本地编码(GBK)，中文会打不开
    QByteArray path = m_filename.toUtf8();
    const char* file_path = path.constData();
    //打开视频文件
    if( avformat_open_input(&pFormatCtx, file_path, NULL, NULL) != 0 )
    {
        qDebug()<<"can't open file:" << m_filename;
        avformat_free_context(pFormatCtx);
        SDL_Quit();
        m_playerState = PlayerState::Stop;
        Q_EMIT SIG_PlayerStateChanged( PlayerState::Stop );
        return;
    }

    //打开成功: 标记已连接, 之后正常读数据不再被超时打断
    netCtx.connected = 1;

    //3.1 获取视频文件信息
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
    {
        qDebug()<<"Could't find stream infomation.";
        avformat_close_input(&pFormatCtx);
        SDL_Quit();
        m_playerState = PlayerState::Stop;
        Q_EMIT SIG_PlayerStateChanged( PlayerState::Stop );
        return;
    }

    //4. 查找音频视频流索引(纯音频/纯视频文件也要能播放, 所以下面分别判断 != -1)
    if (find_stream_index(pFormatCtx, &videoStream, &audioStream) == -1)
    {
        qDebug()<<"Couldn't find stream index";
        avformat_close_input(&pFormatCtx);
        SDL_Quit();
        m_playerState = PlayerState::Stop;
        Q_EMIT SIG_PlayerStateChanged( PlayerState::Stop );
        return;
    }

    m_videoState.pFormatCtx = pFormatCtx;
    m_videoState.videoStream = videoStream;
    m_videoState.audioStream = audioStream;
    m_videoState.m_player = this;

    //一旦开始解析视频文件, 发送视频总时长信号
    Q_EMIT SIG_TotalTime( getTotalTime() );

    if (videoStream != -1) {
        //5. 查找视频解码器
        pCodecCtx = pFormatCtx->streams[videoStream]->codec;
        pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
        if (pCodec == NULL) {
            qDebug()<< "Codec not found.";
            videoStream = -1; //没有视频解码器就只播声音
            m_videoState.videoStream = -1;
            pCodecCtx = NULL;
        } else {
            //打开解码器
            if(avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
                qDebug()<< "Could not open codec.";
                videoStream = -1;
                m_videoState.videoStream = -1;
                pCodecCtx = NULL;
            }
        }
    }
    if( m_videoState.videoStream != -1 )
    {
        //视频流
        m_videoState.video_st = pFormatCtx->streams[ videoStream ];
        m_videoState.pCodecCtx = pCodecCtx;
        //视频同步队列
        m_videoState.videoq = new PacketQueue;
        packet_queue_init( m_videoState.videoq );
        //创建视频线程
        m_videoState.video_tid = SDL_CreateThread( video_thread, "video_thread" , &m_videoState );
    }

    if (audioStream != -1) {
        //5. 找到对应的音频解码器
        pAudioCodecCtx = pFormatCtx->streams[audioStream]->codec;
        pAudioCodec = avcodec_find_decoder(pAudioCodecCtx->codec_id);
        if (!pAudioCodec)
        {
            qDebug()<< "Couldn't find decoder";
            audioStream = -1; //没有音频解码器就只播画面
            m_videoState.audioStream = -1;
            pAudioCodecCtx = NULL;
        }
        else if( avcodec_open2(pAudioCodecCtx, pAudioCodec, NULL) < 0 )
        {
            qDebug()<< "Could not open audio codec.";
            audioStream = -1;
            m_videoState.audioStream = -1;
            pAudioCodecCtx = NULL;
        }
    }
    if( m_videoState.audioStream != -1 )
    {
        //6.设置音频信息, 用来打开音频设备。
        SDL_LockAudio();
        wanted_spec.freq     = pAudioCodecCtx->sample_rate; //采样频率
        wanted_spec.channels = pAudioCodecCtx->channels;    //通道数
        wanted_spec.silence  = 0;                           //设置静音值
        wanted_spec.samples  = SDL_AUDIO_BUFFER_SIZE;       //采样点
        wanted_spec.callback = audio_callback;              //回调函数
        wanted_spec.userdata = &m_videoState;               //回调函数参数
        //根据音频采样格式选择 SDL 的音频格式
        switch (pFormatCtx->streams[audioStream]->codec->sample_fmt)
        {
        case AV_SAMPLE_FMT_U8:
        case AV_SAMPLE_FMT_U8P:
            wanted_spec.format = AUDIO_U8;
            break;
        default:
            wanted_spec.format = AUDIO_S16SYS;
            break;
        };

        //7.打开音频设备
        m_videoState.audioID = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, 0);
        if( m_videoState.audioID < 1 ) //打开失败返回 0(老版本是 -1)
        {
            qDebug()<< "Couldn't open Audio: " << SDL_GetError() ;
            audioStream = -1;
            m_videoState.audioStream = -1;
            m_videoState.audioID = 0;
            SDL_UnlockAudio();
        }
        else
        {
            //音频流(解码的时候要用)
            m_videoState.audio_st = pFormatCtx->streams[audioStream];
            m_videoState.pAudioCodecCtx = pAudioCodecCtx;
            m_videoState.audio_fmt = spec.format;   //设备实际使用的格式
            m_videoState.out_frame.format =
                    (spec.format == AUDIO_U8) ? AV_SAMPLE_FMT_U8 : AV_SAMPLE_FMT_S16;

            //设置参数，供解码时候用, swr_alloc_set_opts 的 out 部分参数
            m_videoState.out_frame.sample_rate    = spec.freq;
            m_videoState.out_frame.channels       = spec.channels;
            m_videoState.out_frame.channel_layout =
                    av_get_default_channel_layout(spec.channels);

            m_videoState.audioq = new PacketQueue;
            //初始化队列
            packet_queue_init(m_videoState.audioq);
            m_videoState.audioFrame = av_frame_alloc();
            SDL_UnlockAudio();
            // SDL 播放声音 0 播放
            SDL_PauseAudioDevice(m_videoState.audioID,0);
        }
    }

    //8. 循环读取视频帧
    //注意: packet 要在音视频都设置完之后分配, 否则纯音频文件用的是未初始化的 packet
    packet = (AVPacket *) av_malloc(sizeof(AVPacket)); //分配一个 packet
    av_init_packet(packet);
    packet->data = NULL;
    packet->size = 0;

    //读线程正式开始工作
    m_videoState.readThreadFinished = 0;
    m_videoState.videoThreadFinished = ( m_videoState.videoStream == -1 );
    int DelayCount = 0; //读取失败计数

    while(1)
    {
        if( m_videoState.quit ) break;

        //跳转处理: 读线程统一执行 av_seek_frame, 并清空两个队列 + 放入 FLUSH 包
        if( m_videoState.seek_req )
        {
            int stream_index = -1;
            int64_t seek_target = m_videoState.seek_pos;//微秒

            if (m_videoState.videoStream >= 0)
                stream_index = m_videoState.videoStream;
            else if (m_videoState.audioStream >= 0)
                stream_index = m_videoState.audioStream;

            AVRational aVRational = {1, AV_TIME_BASE};
            if (stream_index >= 0) {
                seek_target = av_rescale_q(seek_target, aVRational,
                        pFormatCtx->streams[stream_index]->time_base); //跳转到的位置
            }

            if (av_seek_frame(m_videoState.pFormatCtx, stream_index, seek_target,
                              AVSEEK_FLAG_BACKWARD) < 0) {
                fprintf(stderr, "%s: error while seeking\n", m_filename.toLocal8Bit().constData());
            } else {
                if (m_videoState.audioStream >= 0) {
                    AVPacket *flush_pkt = alloc_flush_packet();
                    packet_queue_flush(m_videoState.audioq); //清除队列
                    packet_queue_put(m_videoState.audioq, flush_pkt); //往队列中存入用来清除的包
                }
                if (m_videoState.videoStream >= 0) {
                    AVPacket *flush_pkt = alloc_flush_packet();
                    packet_queue_flush(m_videoState.videoq); //清除队列
                    packet_queue_put(m_videoState.videoq, flush_pkt); //往队列中存入用来清除的包
                    m_videoState.video_clock = 0; //考虑到向左快退 避免卡死
                }
            }
            m_videoState.seek_req = 0;
            m_videoState.seek_time = m_videoState.seek_pos; //微秒, 用于跳过关键帧到目的时间
            m_videoState.seek_flag_audio = 1;
            m_videoState.seek_flag_video = 1;
        }

        if( m_videoState.isPause ) //暂停
        {
            SDL_Delay(10);
            continue;
        }

        //这里做了个限制 当队列里面的数据超过某个大小的时候 就暂停读取 防止一下子就把
        //视频读完了，导致的空间分配不足
        /* 这里 audioq.size 是指队列中的所有数据包带的音频数据的总量或者视频数据总量，并
        不是包的数量 */
        //这个值可以稍微写大一些
        if( m_videoState.audioStream != -1 && m_videoState.audioq->size > MAX_AUDIO_SIZE ) {
            SDL_Delay(10);
            continue;
        }
        if ( m_videoState.videoStream != -1 && m_videoState.videoq->size > MAX_VIDEO_SIZE) {
            SDL_Delay(10);
            continue;
        }
        //可以看出 av_read_frame 读取的是一帧视频，并存入一个 AVPacket 的结构中
        if (av_read_frame(pFormatCtx, packet) < 0)
        {
            //发现返回<0, 并不需要马上退出, 可以做一下延迟(兼容网络流)
            DelayCount++;
            if( DelayCount >= 300 )
            {
                m_videoState.readFinished = 1;
                DelayCount = 0;
            }
            if( m_videoState.quit ) break; //解码线程执行完 退出
            SDL_Delay(10);

            //文件读取完毕, 且两个队列中的数据都被解码线程取完了, 结束读取线程
            if( m_videoState.readFinished
                    && ( m_videoState.videoStream == -1 || m_videoState.videoq->nb_packets == 0 )
                    && ( m_videoState.audioStream == -1 || m_videoState.audioq->nb_packets == 0 ) )
            {
                break;
            }
            continue;
        }
        DelayCount = 0;
        //生成图片
        if (packet->stream_index == m_videoState.videoStream)
        {
            packet_queue_put(m_videoState.videoq, packet);
        }
        else if ( packet->stream_index == m_videoState.audioStream)
        {
            packet_queue_put(m_videoState.audioq, packet);
        }
        else
        {
            av_packet_unref(packet); //新版使用 av_packet_unref() 来代替 av_free_packet()
        }
    }

    //9. 等待队列中的数据播放完
    while( m_videoState.videoStream != -1 && m_videoState.videoq->nb_packets != 0 )
    {
        if( m_videoState.quit ) break;
        SDL_Delay(100);
    }
    SDL_Delay(100);
    while( m_videoState.audioStream != -1 && m_videoState.audioq->nb_packets != 0 )
    {
        if( m_videoState.quit ) break;
        SDL_Delay(100);
    }
    SDL_Delay(100);

    //10. 通知解码线程退出(阻塞在 packet_queue_get 上的视频线程靠这个唤醒)
    m_videoState.quit = 1;
    if( m_videoState.videoq ) packet_queue_abort( m_videoState.videoq );
    if( m_videoState.audioq ) packet_queue_abort( m_videoState.audioq );
    //等待视频解码线程真正结束, 否则后面释放解码器会崩
    while( m_videoState.videoStream != -1 && !m_videoState.videoThreadFinished )
    {
        SDL_Delay(10);
    }
    //关闭音频设备
    if( m_videoState.audioID )
    {
        SDL_LockAudio();
        SDL_PauseAudioDevice( m_videoState.audioID , 1 ); //停止播放,即停止音频回调函数
        SDL_CloseAudioDevice( m_videoState.audioID );
        SDL_UnlockAudio();
        m_videoState.audioID = 0;
    }

    //11.回收空间
    if( m_videoState.videoq )
    {
        packet_queue_flush( m_videoState.videoq ); //队列回收
        packet_queue_destroy( m_videoState.videoq );
        delete m_videoState.videoq;
        m_videoState.videoq = NULL;
    }
    if( m_videoState.audioq )
    {
        packet_queue_flush( m_videoState.audioq ); //队列回收
        packet_queue_destroy( m_videoState.audioq );
        delete m_videoState.audioq;
        m_videoState.audioq = NULL;
    }
    if( m_videoState.audioFrame )
    {
        av_frame_free( &m_videoState.audioFrame );
        m_videoState.audioFrame = NULL;
    }
    if( audioStream != -1 && pAudioCodecCtx )
        avcodec_close(pAudioCodecCtx);
    if( videoStream != -1 && pCodecCtx )
        avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatCtx);
    if( packet )
    {
        av_packet_unref(packet);
        av_free(packet);
    }
    SDL_Quit();

    //回收资源之后, 设置读取文件线程退出标志
    m_videoState.readThreadFinished = 1;
    //视频自动结束 置标志位
    m_playerState = PlayerState::Stop;
    Q_EMIT SIG_PlayerStateChanged( PlayerState::Stop );
    qDebug() << "VideoPlayer::run end" ;
}

//13.回调函数中将从队列中取数据, 解码后填充到播放缓冲区.
static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    VideoState * is = (VideoState *) userdata;
    int len1, audio_data_size;
    if( !is ) return;

    //暂停时清空缓冲区, 避免一直播放上一帧声音
    memset( stream , 0 , len);
    if( is->isPause ) return;

/* len 是由 SDL 传入的 SDL 缓冲区的大小，如果这个缓冲未满，我们就一直往里填充数据 */

/* audio_buf_index 和 audio_buf_size 标示我们自己用来放置解码出来的数据的缓冲区，*/
/* 这些数据待 copy 到 SDL 缓冲区， 当 audio_buf_index >= audio_buf_size 的时候意味着我*/
/* 们的缓冲为空，没有数据可供 copy，这时候需要调用 audio_decode_frame 来解码出更
/* 多的桢数据 */
    while (len > 0)
    {
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_data_size = audio_decode_frame( is ,
                                                  is->audio_buf,sizeof(is->audio_buf));
            /* audio_data_size < 0 标示没能解码出数据，我们默认播放静音 */
            if (audio_data_size < 0) {
                /* silence */
                is->audio_buf_size = 1024;
                /* 清零，静音 */
                memset(is->audio_buf, 0, is->audio_buf_size);
            } else {
                is->audio_buf_size = audio_data_size;
            }
            is->audio_buf_index = 0;
        }
        /* 查看 stream 可用空间，决定一次 copy 多少数据，剩下的下次继续 copy */
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len) {
            len1 = len;
        }
        memset( stream , 0 , len1);
        //混音函数 sdl 2.0 版本使用该函数 替换 SDL_MixAudio (最后一个参数为音量)
        SDL_MixAudioFormat(stream, (uint8_t *) is->audio_buf + is->audio_buf_index,
                           is->audio_fmt, len1, is->volume);
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}

//对于音频来说，一个 packet 里面，可能含有多帧(frame)数据。
//解码音频帧函数
static int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size)
{
    AVPacket pkt;
    int data_size = 0;
    AVCodecContext *aCodecCtx = is->pAudioCodecCtx;
    AVFrame *audioFrame = is->audioFrame;
    PacketQueue *audioq = is->audioq;
    struct SwrContext *swr_ctx = NULL;

    if( !aCodecCtx || !audioFrame || !audioq ) return -1;

    for(;;)
    {
        if( is->quit ) return -1;
        if(packet_queue_get(audioq, &pkt, 0) <= 0) //一定注意
        {
            if( is->readFinished && audioq->nb_packets == 0 )
                is->quit = 1; //播放到结尾
            return -1;
        }

        //收到 FLUSH_DATA 说明刚刚执行过跳转, 清空音频解码器缓存
        if( strcmp((char*)pkt.data, FLUSH_DATA) == 0 )
        {
            avcodec_flush_buffers( is->pAudioCodecCtx );
            av_packet_unref( &pkt );
            continue;
        }

        //计算音频时钟(单位: 微秒), 视频以它为主时钟
        if( pkt.pts != AV_NOPTS_VALUE && is->audio_st )
        {
            is->audio_clock = (double)pkt.pts * av_q2d( is->audio_st->time_base ) * 1000000.0;
        }

        //跳转到关键帧, 跳过一些帧(没有到目的时间就不播)
        if( is->seek_flag_audio )
        {
            if( is->audio_clock < is->seek_time ) //没有到目的时间
            {
                av_packet_unref( &pkt );
                return -1;
            }
            else
            {
                is->seek_flag_audio = 0;
            }
        }

        int got_picture = 0;
        av_frame_unref( audioFrame ); //复用同一个 AVFrame
        int ret = avcodec_decode_audio4( aCodecCtx, audioFrame, &got_picture, &pkt);
        av_packet_unref( &pkt );      //新版使用 av_packet_unref() 来代替 av_free_packet()
        if( ret < 0 ) {
            qDebug() << "Error in decoding audio frame.";
            continue; //解码失败的包丢掉, 不能 exit(0)
        }
        if( !got_picture || audioFrame->nb_samples <= 0 )
        {
            continue;
        }

        //一个采样点占用的字节数: U8 是 1 个字节, S16 是 2 个字节
        int bytes_per_sample = ( is->out_frame.format == AV_SAMPLE_FMT_U8 ) ? 1 : 2;
        int out_channels = is->out_frame.channels > 0 ? is->out_frame.channels : 2;
        //buf_size 是字节数, 换算成"每声道最多能放多少个采样点", 防止写越界
        int out_max_samples = buf_size / ( out_channels * bytes_per_sample );
        uint64_t out_layout = is->out_frame.channel_layout ?
                    is->out_frame.channel_layout :
                    av_get_default_channel_layout( out_channels );
        uint64_t in_layout = audioFrame->channel_layout ?
                    audioFrame->channel_layout :
                    av_get_default_channel_layout( audioFrame->channels );

        swr_ctx = swr_alloc_set_opts(NULL, out_layout,
                                     (AVSampleFormat)is->out_frame.format,
                                     is->out_frame.sample_rate,
                                     in_layout,
                                     (AVSampleFormat)audioFrame->format,
                                     audioFrame->sample_rate, 0, NULL);
        //初始化
        if (swr_ctx == NULL || swr_init(swr_ctx) < 0)
        {
            qDebug() << "swr_init error";
            if( swr_ctx ) swr_free( &swr_ctx );
            continue;
        }
        int convert_len = swr_convert(swr_ctx, &audio_buf, out_max_samples,
                                      (const uint8_t **)audioFrame->data,
                                      audioFrame->nb_samples);
        swr_free( &swr_ctx );
        if( convert_len <= 0 ) continue;

        //重采样之后的数据大小: 采样点数 * 声道数 * 每个采样点字节数
        data_size = convert_len * out_channels * bytes_per_sample;
        if( data_size > buf_size ) data_size = buf_size;
        return data_size ;
    }
}

//查找数据流函数
static int find_stream_index(AVFormatContext *pformat_ctx, int *video_stream, int
                             *audio_stream)
{
    assert(video_stream != NULL || audio_stream != NULL);
    int i = 0;
    int audio_index = -1;
    int video_index = -1;
    for (i = 0; i < (int)pformat_ctx->nb_streams; i++)
    {
        if (pformat_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            video_index = i;
        }
        if (pformat_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audio_index = i;
        }
    }
    //注意以下两个判断有可能返回-1.
    if (video_stream == NULL)
    {
        *audio_stream = audio_index;
        return *audio_stream;
    }
    if (audio_stream == NULL)
    {
        *video_stream = video_index;
        return *video_stream;
    }
    *video_stream = video_index;
    *audio_stream = audio_index;
    return 0;
}
