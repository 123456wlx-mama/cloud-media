#define SDL_MAIN_HANDLED 1

#include "player/PlayerCore.h"

#include <QDateTime>
#include <QFileInfo>
#include <QTimer>
#include <QtGlobal>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <SDL.h>
}

#include "player/GrowingFileIO.h"
#include "player/PacketQueue.h"

namespace cm {
namespace player {
namespace {

const int kAudioBufferBytes = 192000 * 4;  // 一帧 PCM 上限（S16 立体声）
const int kTickMs = 200;
// 音画同步阈值（面试要能解释清楚，全部具名）：
// - 视频比音频早 <=8ms：不等待（容差，避免每帧都 sleep）
// - 视频比音频晚 >120ms：丢帧追赶（人眼对"画面落后"更敏感，宁可丢帧也不拖音频）
// - seek 后 pts 早于目标 20ms 内的帧视为旧位置残留，直接丢
const double kSyncSlopMs = 8.0;
const double kVideoDropMs = 120.0;
const double kSeekDropSlopMs = 20.0;
const double kAudioSampleRate = 44100.0;
// 视频包队列上限（字节）：约 2~3 秒缓冲，防止完整文件被一次性读进内存
const int kMaxQueuedVideoBytes = 8 * 1024 * 1024;

std::string avErrorText(int code) {
    char buf[256];
    buf[0] = '\0';
    av_strerror(code, buf, sizeof(buf));
    return std::string(buf);
}

qint64 nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

}  // namespace

struct PlayerCore::Impl {
    explicit Impl(PlayerCore* owner)
        : q(owner), progressive(false), expectedSize(0), io(nullptr),
          fmt(nullptr),
          videoIndex(-1), audioIndex(-1), vctx(nullptr), actx(nullptr),
          vstream(nullptr), astream(nullptr), sws(nullptr), swr(nullptr),
          swsW(0), swsH(0), swsTW(0), swsTH(0), swsFmt(AV_PIX_FMT_NONE), dispW(0),
          dispH(0), frameInFlight(false),
          quit(false), paused(false), seekReq(false), seekTargetMs(0),
          positionMs(0), durationMs(0), audioClockMs(0.0), audioClockValid(false),
          audioFramePts(0.0), audioFrameConsumed(0), seekDropMs(0), seekGen(0),
          audioFlushedGen(0), pktLogs(0),
          droppedFrames(0),
          wallStarted(false), wallStartMs(0.0), wallStartPts(0.0), volume(80),
          firstFrame(false), readFinished(false), videoDone(false),
          endedEmitted(false), audioDev(0), aFrame(nullptr), audioBufSize(0),
          audioBufIndex(0), tick(nullptr), lastState(PlayerCore::Idle) {}

    PlayerCore* q;
    QString path;
    bool progressive;
    qint64 expectedSize;  // 远端真实总大小；0=未知（本地文件）

    GrowingFileIO* io;
    AVFormatContext* fmt;
    int videoIndex;
    int audioIndex;
    AVCodecContext* vctx;
    AVCodecContext* actx;
    AVStream* vstream;
    AVStream* astream;
    SwsContext* sws;
    SwrContext* swr;
    int swsW;
    int swsH;
    int swsTW;  // sws 当前输出的目标宽度（已按画布缩放；等于 swsW 表示不缩）
    int swsTH;
    AVPixelFormat swsFmt;  // sws 当前适配的源格式（可能按帧变化）

    // 画布设备像素尺寸（0,0 = 不知道，按源尺寸出帧）
    std::atomic<int> dispW;
    std::atomic<int> dispH;
    // 已交付但 GUI 还没消费的帧。置位期间不再交付新帧（丢帧背压）：
    // QImage 跨线程是队列连接 + 深拷贝，GUI 一慢（全屏软件重绘、拖窗口、
    // 弹模态框）事件队列里就会堆几十份 ARGB 大图，32 位进程地址空间撑不住，
    // 最终表现为 CreateDIBSection / malloc 失败然后整个进程异常退出。
    std::atomic<bool> frameInFlight;

    PacketQueue videoQ;
    PacketQueue audioQ;
    std::thread demuxThread;
    std::thread videoThread;

    std::atomic<bool> quit;
    std::atomic<bool> paused;
    std::atomic<bool> seekReq;
    std::atomic<qint64> seekTargetMs;
    std::atomic<qint64> positionMs;
    std::atomic<qint64> durationMs;
    std::atomic<double> audioClockMs;
    std::atomic<bool> audioClockValid;
    // 音频时钟按"已交给声卡的样本数"推进：帧起点 pts + 已消费样本/采样率
    std::atomic<double> audioFramePts;
    std::atomic<int> audioFrameConsumed;
    // seek 屏障：早于该时刻的音频/视频帧直接丢弃（0=无）
    std::atomic<qint64> seekDropMs;
    // seek 代数：seek 时自增；解码线程发现"包的代数"变了就丢弃手上海帧（防回跳）
    std::atomic<int> seekGen;
    // 音频解码线程自己记住"已 flush 到哪一代"（只在 SDL 回调线程读写）
    int audioFlushedGen;
    int pktLogs;  // 诊断：已打印过几个视频包
    std::atomic<int> droppedFrames;
    std::atomic<bool> wallStarted;
    std::atomic<double> wallStartMs;
    std::atomic<double> wallStartPts;
    std::atomic<int> volume;
    std::atomic<bool> firstFrame;
    std::atomic<bool> readFinished;
    std::atomic<bool> videoDone;
    std::atomic<bool> endedEmitted;

    SDL_AudioDeviceID audioDev;
    AVFrame* aFrame;
    std::mutex audioMutex;
    std::vector<std::uint8_t> audioBuf;
    int audioBufSize;
    int audioBufIndex;

    std::vector<std::uint8_t> rgbBuf;
    QImage lastImage;
    QString errorText;
    QTimer* tick;
    int lastState;

    // ---------------------------------------------------------------- 生命周期
    bool openMedia(const QString& filePath, bool prog, QString* err);
    void closeMedia();
    void startThreads();
    void joinThreads();

    // ---------------------------------------------------------------- 线程体
    void demuxLoop();
    void videoLoop();
    // 线程入口包一层：std::thread 里逃出的异常会直接 std::terminate，
    // 界面上只能看到"程序异常结束"，连一条 Qt 日志都没有。
    void demuxLoopGuarded();
    void videoLoopGuarded();
    void doSeek();
    void fail(const QString& text);
    // 出帧尺寸：按画布设备像素等比缩（只缩不放），再套一个像素上限
    void computeFrameSize(int srcW, int srcH, int* outW, int* outH) const;

    int decodeAudio(std::uint8_t* out, int maxBytes);
    void fillAudio(std::uint8_t* stream, int len);
    static void sdlAudioCallback(void* userdata, std::uint8_t* stream, int len);
    static int interruptCb(void* opaque);

    bool deliverFrame(AVFrame* frame);
    void syncToClock(double ptsMs);
    double referenceClockMs();
    qint64 currentPositionMs();
    void resetWallClock(double ptsMs);
    void setState(int state);
    int openCodec(int streamIndex, AVCodecContext** ctxOut, bool video);
};

bool PlayerCore::Impl::openMedia(const QString& filePath, bool prog, QString* err) {
    closeMedia();
    // closeMedia() 会把 quit 置 true 用来叫停上一次的线程。这里必须立刻复位：
    // open / find_stream_info 期间 interrupt_callback 会读这个标志，留在 true
    // 会让 FFmpeg 认为"用户要中止"，find_stream_info 直接被截断——
    // 典型症状是控制台刷 "Could not find codec parameters ... unspecified pixel format"，
    // codecpar->format 停在 -1，等于拿半套参数去开解码器。
    quit = false;
    path = filePath;
    progressive = prog;

    io = new GrowingFileIO(filePath, prog, expectedSize);
    if (!io->init(err)) {
        delete io;
        io = nullptr;
        return false;
    }
    io->setStallCallback([this](bool stalled) {
        if (q != nullptr) {
            q->bufferingChanged(stalled);
        }
    });
    qWarning("player: io ready readable=%lld total=%lld",
             static_cast<long long>(io->readableBytes()),
             static_cast<long long>(expectedSize));

    fmt = avformat_alloc_context();
    if (fmt == nullptr) {
        *err = QString::fromUtf8("分配格式上下文失败");
        return false;
    }
    fmt->pb = io->context();
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    // 边下边播时只能读到已下载的部分：限制探测/分析，避免 open 阶段死等后面的数据
    fmt->probesize = 512 * 1024;
    fmt->max_analyze_duration = 2 * AV_TIME_BASE;
    fmt->interrupt_callback.callback = &Impl::interruptCb;
    fmt->interrupt_callback.opaque = this;

    const QByteArray utf8Path = filePath.toUtf8();
    int rc = avformat_open_input(&fmt, utf8Path.constData(), nullptr, nullptr);
    if (rc < 0) {
        // AVERROR_EXIT 来自 GrowingFileIO 的停滞保护：下载卡住时主动放弃，
        // 而不是让 GUI 线程无限等待（表现为窗口未响应）。
        *err = (rc == AVERROR_EXIT)
                   ? QString::fromUtf8("下载停滞，无法打开（已下载的数据不够定位文件头）")
                   : QString::fromUtf8("打开失败：%1")
                         .arg(QString::fromStdString(avErrorText(rc)));
        return false;
    }
    rc = avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) {
        *err = (rc == AVERROR_EXIT)
                   ? QString::fromUtf8("下载停滞，读取流信息超时")
                   : QString::fromUtf8("读取流信息失败：%1")
                         .arg(QString::fromStdString(avErrorText(rc)));
        return false;
    }
    // 探测会把 pb 往前推（probesize 512KB）。对「moov 里带完整 stco」的普通 MP4
    // 无所谓，demuxer 每个 sample 都按绝对偏移读；但对**分片 MP4（moof+mdat，
    // HEVC 转码常见）**是致命的：moov 里没有索引，demuxer 只能从 pb 当前位置
    // 往后顺序扫 moof —— 开头那个分片被跳过，第一个包直接落到几 MB 之后，
    // 而且不是关键帧，于是满屏 "Invalid NAL unit 4" / "Invalid NAL unit 50"。
    // 所以探测完必须显式复位到文件开头。
    if (av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
        // 复位失败不致命：普通 MP4 仍能靠 stco 正常播
        qWarning("player: seek to start failed after probing");
    }

    AVCodec* vcodec = nullptr;
    AVCodec* acodec = nullptr;
    videoIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &vcodec, 0);
    audioIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &acodec, 0);
    if (videoIndex < 0 && audioIndex < 0) {
        *err = QString::fromUtf8("文件里没有可播放的音视频流");
        return false;
    }
    if (videoIndex >= 0 && !openCodec(videoIndex, &vctx, true)) {
        *err = QString::fromUtf8("视频解码器打开失败");
        return false;
    }
    if (audioIndex >= 0 && !openCodec(audioIndex, &actx, false)) {
        *err = QString::fromUtf8("音频解码器打开失败");
        return false;
    }
    vstream = videoIndex >= 0 ? fmt->streams[videoIndex] : nullptr;
    astream = audioIndex >= 0 ? fmt->streams[audioIndex] : nullptr;

    if (fmt->duration > 0) {
        durationMs = fmt->duration / (AV_TIME_BASE / 1000);
    } else if (vstream != nullptr && vstream->duration > 0) {
        durationMs = vstream->duration * av_q2d(vstream->time_base) * 1000.0;
    }

    // 音频输出：固定 44.1k / S16 / 立体声，解码侧用 swr 转
    if (audioIndex >= 0 && actx != nullptr) {
        const std::uint64_t inLayout =
            actx->channel_layout != 0
                ? actx->channel_layout
                : av_get_default_channel_layout(actx->channels);
        swr = swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16,
                                 44100, static_cast<std::int64_t>(inLayout),
                                 actx->sample_fmt, actx->sample_rate, 0, nullptr);
        if (swr == nullptr || swr_init(swr) < 0) {
            *err = QString::fromUtf8("音频重采样初始化失败");
            return false;
        }
        aFrame = av_frame_alloc();
        audioBuf.resize(kAudioBufferBytes);
        audioBufSize = 0;
        audioBufIndex = 0;

        SDL_AudioSpec want;
        SDL_zero(want);
        want.freq = 44100;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = &Impl::sdlAudioCallback;
        want.userdata = this;
        SDL_AudioSpec have;
        SDL_zero(have);
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            audioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        }
        if (audioDev == 0) {
            // 没有声卡也能播画面：退回系统时钟同步
            qWarning("player: audio device unavailable: %s", SDL_GetError());
        }
    }

    positionMs = 0;
    // 这几项必须归零：它们记录的是「上一部片子」的 seek 状态。
    // seekDropMs 是 seek 后的丢帧屏障（丢弃 pts 早于它的帧）——
    // 若沿用上一次拖到的 12 秒，新片子前 12 秒会被整段丢掉，
    // 表现就是「刚打开就显示 00:12」并且开头卡一下。
    // 注意：durationMs 不能在这里清零 —— 它在上面已经由 fmt->duration 算好了，
    // 清零会让进度条拿到 0 时长而变灰、拖不动。closeMedia() 里已负责重置。
    seekTargetMs = 0;
    seekDropMs = 0;
    seekGen = 0;
    audioFlushedGen = 0;
    droppedFrames = 0;
    // 音频主时钟必须归零。它是「上一部片子」播到哪的绝对值：
    // 新片子音频从 pts=0 开始，而 decodeAudio 里「时钟单调不回退」会让它停在旧值
    // （0 > 52000 不成立），于是视频帧 0~52s 全被判成「落后音频」丢掉，
    // 画面卡在旧位置的最后一帧不动，声音却是从头播的。
    audioClockMs = 0.0;
    audioFramePts = 0.0;
    audioFrameConsumed = 0;
    firstFrame = false;
    readFinished = false;
    videoDone = false;
    endedEmitted = false;
    audioClockValid = false;
    wallStarted = false;
    seekReq = false;
    paused = false;
    quit = false;
    errorText.clear();
    videoQ.reset();
    audioQ.reset();

    startThreads();
    if (audioDev != 0) {
        SDL_PauseAudioDevice(audioDev, 0);
    }
    // 诊断：duration 为 0 会让进度条变灰拖不动，一行日志就能定位
    qWarning("player: ready duration=%lldms %dx%d progressive=%d",
             static_cast<long long>(durationMs.load()),
             vctx != nullptr ? vctx->width : 0, vctx != nullptr ? vctx->height : 0,
             progressive ? 1 : 0);
    q->opened(durationMs.load(), vctx != nullptr ? vctx->width : 0,
              vctx != nullptr ? vctx->height : 0);
    return true;
}

int PlayerCore::Impl::openCodec(int streamIndex, AVCodecContext** ctxOut, bool video) {
    AVStream* stream = fmt->streams[streamIndex];
    AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        return 0;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (ctx == nullptr) {
        return 0;
    }
    if (avcodec_parameters_to_context(ctx, stream->codecpar) < 0) {
        avcodec_free_context(&ctx);
        return 0;
    }
    // 注意：codecpar->format 是 find_stream_info 探测出来的，边下边播时
    // 数据不足就可能是 -1，不代表解码器不可用。真正要看的是 avcodec_open2
    // 之后 ctx->pix_fmt 是否被 SPS 填上。
    qWarning("player: codec=%s codecpar(pix_fmt=%d extradata=%d) %dx%d",
             avcodec_get_name(stream->codecpar->codec_id),
             static_cast<int>(stream->codecpar->format),
             stream->codecpar->extradata_size, stream->codecpar->width,
             stream->codecpar->height);

    ctx->pkt_timebase = stream->time_base;
    if (video) {
        ctx->thread_count = 2;  // 32 位进程别开太多解码线程
    }
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return 0;
    }
    // 解码器初始化后的真实状态：pix_fmt 有效说明 SPS 解析成功。
    qWarning("player: opened(%s) ctx_pix_fmt=%d extradata=%d", video ? "video" : "audio",
             ctx->pix_fmt, ctx->extradata_size);
    *ctxOut = ctx;
    return 1;
}

void PlayerCore::Impl::startThreads() {
    demuxThread = std::thread(&Impl::demuxLoopGuarded, this);
    videoThread = std::thread(&Impl::videoLoopGuarded, this);
}

void PlayerCore::Impl::joinThreads() {
    if (demuxThread.joinable()) {
        demuxThread.join();
    }
    if (videoThread.joinable()) {
        videoThread.join();
    }
}

void PlayerCore::Impl::demuxLoopGuarded() {
    try {
        demuxLoop();
    } catch (const std::exception& e) {
        qWarning("player: demux thread threw: %s", e.what());
        readFinished = true;
        q->errorChanged(QString::fromUtf8("读取失败：%1").arg(QString::fromUtf8(e.what())));
    } catch (...) {
        qWarning("player: demux thread threw unknown exception");
        readFinished = true;
    }
}

void PlayerCore::Impl::videoLoopGuarded() {
    try {
        videoLoop();
    } catch (const std::exception& e) {
        qWarning("player: video thread threw: %s", e.what());
        videoDone = true;
        frameInFlight = false;
        q->errorChanged(QString::fromUtf8("解码失败：%1").arg(QString::fromUtf8(e.what())));
    } catch (...) {
        qWarning("player: video thread threw unknown exception");
        videoDone = true;
        frameInFlight = false;
    }
}

void PlayerCore::Impl::closeMedia() {
    if (fmt != nullptr) {
        qWarning("player: closeMedia(%p)", static_cast<void*>(q));
    }
    quit = true;
    paused = false;
    audioQ.abort();
    videoQ.abort();
    // abort 只唤醒阻塞者并把后续 get 挡掉，队列里的包必须显式清：
    // 不清就会带着上一部片子的残留包进入下一次 open，
    // 新解码器（编码格式甚至不同）消费旧包 → Invalid NAL / 上一部的声音。
    audioQ.flush();
    videoQ.flush();
    if (io != nullptr) {
        io->abort();
    }
    joinThreads();
    if (audioDev != 0) {
        SDL_CloseAudioDevice(audioDev);
        audioDev = 0;
    }
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        audioBufSize = 0;
        audioBufIndex = 0;
    }
    if (aFrame != nullptr) {
        av_frame_free(&aFrame);
    }
    if (swr != nullptr) {
        swr_free(&swr);
    }
    if (sws != nullptr) {
        sws_freeContext(sws);
        sws = nullptr;
    }
    swsFmt = AV_PIX_FMT_NONE;
    swsW = 0;
    swsH = 0;
    swsTW = 0;
    swsTH = 0;
    // 上一部片子的最后一帧可能压在事件队列里没人消费；不复位的话
    // 下一部片子会被背压永久挡死（表现：有声音、一帧不出）。
    frameInFlight = false;
    pktLogs = 0;
    if (vctx != nullptr) {
        avcodec_free_context(&vctx);
    }
    if (actx != nullptr) {
        avcodec_free_context(&actx);
    }
    if (fmt != nullptr) {
        // 自定义 IO：avformat_close_input 不会释放 pb，由 GrowingFileIO 析构处理
        avformat_close_input(&fmt);
    }
    delete io;
    io = nullptr;
    videoIndex = -1;
    audioIndex = -1;
    vstream = nullptr;
    astream = nullptr;
    audioClockValid = false;
    readFinished = false;
    videoDone = false;
    firstFrame = false;
    positionMs = 0;
    durationMs = 0;
}

void PlayerCore::Impl::setState(int state) {
    if (lastState == state) {
        return;
    }
    lastState = state;
    q->stateChanged(state);
}

void PlayerCore::Impl::fail(const QString& text) {
    errorText = text;
    q->errorChanged(text);
}

int PlayerCore::Impl::interruptCb(void* opaque) {
    Impl* self = static_cast<Impl*>(opaque);
    return (self->quit.load() || (self->io != nullptr && self->io->aborted())) ? 1 : 0;
}

void PlayerCore::Impl::doSeek() {
    const qint64 targetMs = seekTargetMs.load();
    const std::int64_t ts = static_cast<std::int64_t>(targetMs) * 1000;
    const int rc = av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        qWarning("player: seek failed: %s", avErrorText(rc).c_str());
        return;
    }
    videoQ.flush();
    audioQ.flush();
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        audioBufSize = 0;
        audioBufIndex = 0;
    }
    // 这里绝不能调 avcodec_flush_buffers：vctx 归视频解码线程、actx 归 SDL 音频回调线程，
    // 在 demux 线程去 flush 就是和 send_packet / receive_frame 并发的数据竞争，
    // 会把参考帧队列搅乱（大面积 "reference picture missing" / "Reference N >= M"）。
    // 只递增代数，由各自线程在下次取包前自行 flush。
    ++seekGen;  // 与 flush 同一时刻生效：此后解码出的旧包帧全部作废
    positionMs = targetMs;
    audioClockMs = static_cast<double>(targetMs);
    audioClockValid = audioIndex >= 0;
    audioFramePts = static_cast<double>(targetMs);
    audioFrameConsumed = 0;
    seekDropMs = targetMs;  // 早于目标的旧帧一律丢弃，避免 seek 后画面/声音回跳
    wallStarted = false;
    readFinished = false;
    endedEmitted = false;
}

void PlayerCore::Impl::demuxLoop() {
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        return;
    }
    // 连续读失败上限：progressive 下每次失败等 50ms，2s 还读不出来就不是"数据还没到"
    // （那种情况 GrowingFileIO 是阻塞等待，不会返回错误），而是数据已经损坏。
    const int kMaxReadFail = 40;
    int readFailCount = 0;
    while (!quit) {
        if (seekReq.exchange(false)) {
            doSeek();
        }
        const int rc = av_read_frame(fmt, packet);
        if (rc < 0) {
            if (rc == AVERROR_EXIT) {
                av_packet_unref(packet);
                break;
            }
            const bool eof = (rc == AVERROR_EOF) ||
                             (io != nullptr && io->sourceComplete());
            if (eof) {
                readFinished = true;
                // 等播放线程吃完 / 用户 seek / 关闭
                while (!quit && !seekReq.exchange(false)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (quit) {
                    break;
                }
                readFinished = false;
                doSeek();
                continue;
            }
            if (progressive) {
                // 文件还没写完时可能出现短暂的读失败，等下一块；
                // 但持续失败必须放弃，否则坏了会无限重试：日志刷屏 + CPU 空转。
                if (++readFailCount >= kMaxReadFail) {
                    fail(QString::fromUtf8("读取失败（连续 %1 次）：%2")
                             .arg(readFailCount)
                             .arg(QString::fromStdString(avErrorText(rc))));
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            fail(QString::fromUtf8("读取失败：%1")
                     .arg(QString::fromStdString(avErrorText(rc))));
            break;
        }
        readFailCount = 0;
        if (packet->stream_index == videoIndex) {
            // 队列必须有上限：完整文件下 demux 会在几百毫秒内把整片读进内存
            // （20MB 片子 = 18000 个包），既浪费内存，也让「开关字幕/切集」
            // 这类操作突然要等一大堆积压。限到约 2~3 秒的缓冲量。
            // 等待期间必须能响应 seek：否则暂停时队列一直满，
            // 用户拖进度条会像「拖不动」一样毫无反应。
            bool seekWhileFull = false;
            while (!quit && videoQ.bytes() > kMaxQueuedVideoBytes) {
                if (seekReq.load()) {
                    seekWhileFull = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (quit) {
                av_packet_unref(packet);
                break;
            }
            if (seekWhileFull) {
                av_packet_unref(packet);
                continue;  // 回主循环，先处理 seekReq
            }
            videoQ.put(packet);
        } else if (packet->stream_index == audioIndex) {
            audioQ.put(packet);
        } else {
            av_packet_unref(packet);
        }
    }
    av_packet_free(&packet);
    readFinished = true;
}

double PlayerCore::Impl::referenceClockMs() {
    if (audioDev != 0 && audioClockValid.load()) {
        return audioClockMs.load();
    }
    const double base = wallStartPts.load();
    const double started = wallStartMs.load();
    return base + (static_cast<double>(nowMs()) - started);
}

void PlayerCore::Impl::resetWallClock(double ptsMs) {
    wallStartPts = ptsMs;
    wallStartMs = static_cast<double>(nowMs());
    wallStarted = true;
}

void PlayerCore::Impl::syncToClock(double ptsMs) {
    // 无音频（或音频时钟还没起来）时用系统时钟做基准。锚点必须等"确认有效"的帧
    // 交付前再设（videoLoop 里做），否则 seek 时一张旧帧会把基准锚回旧位置。
    if ((audioDev == 0 || !audioClockValid.load()) && !wallStarted.load()) {
        return;
    }
    for (int i = 0; i < 400 && !quit; ++i) {
        if (paused) {
            return;
        }
        const double diff = ptsMs - referenceClockMs();
        if (diff <= kSyncSlopMs) {
            return;
        }
        if (diff > 2000.0) {
            return;  // 时钟异常，别把画面卡死
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void PlayerCore::Impl::computeFrameSize(int srcW, int srcH, int* outW,
                                        int* outH) const {
    // 出帧像素上限：再大只是白烧内存带宽（1080p ≈ 207 万像素，够 2K 屏 100% 显示）
    const double kMaxFramePixels = 1920.0 * 1200.0;
    int tw = srcW;
    int th = srcH;
    const int dw = dispW.load();
    const int dh = dispH.load();
    if (dw > 0 && dh > 0) {
        // 等比适配画布（黑边由渲染层补），只缩不放
        const double sx = static_cast<double>(dw) / static_cast<double>(srcW);
        const double sy = static_cast<double>(dh) / static_cast<double>(srcH);
        const double scale = sx < sy ? sx : sy;
        if (scale < 0.95) {  // 轻微缩放不值得付重采样代价
            tw = static_cast<int>(static_cast<double>(srcW) * scale);
            th = static_cast<int>(static_cast<double>(srcH) * scale);
        }
    }
    const double pixels = static_cast<double>(tw) * static_cast<double>(th);
    if (pixels > kMaxFramePixels) {
        const double k = std::sqrt(kMaxFramePixels / pixels);
        tw = static_cast<int>(static_cast<double>(tw) * k);
        th = static_cast<int>(static_cast<double>(th) * k);
    }
    if (tw < 2) {
        tw = 2;
    }
    if (th < 2) {
        th = 2;
    }
    tw &= ~1;  // 偶数宽高：纹理/swscale 都更省
    th &= ~1;
    *outW = tw;
    *outH = th;
}

bool PlayerCore::Impl::deliverFrame(AVFrame* frame) {
    if (vctx == nullptr || frame->width <= 0 || frame->height <= 0) {
        return false;
    }
    // 背压：上一帧交付后 GUI 还没消费（还压在事件队列里），这一帧直接丢。
    // 宁可掉帧也不能让队列无限堆积大图——那是 32 位客户端被 OOM 带走的主因。
    if (frameInFlight.load()) {
        ++droppedFrames;
        return false;
    }
    const int w = frame->width;
    const int h = frame->height;
    // 源格式一律取「这一帧自己的 format」。
    // vctx->pix_fmt 在 find_stream_info 没探测出参数时是 -1（AV_PIX_FMT_NONE），
    // 拿它去 sws_getContext 会直接失败——表现就是有声音、一帧画面都不出。
    const AVPixelFormat srcFmt = static_cast<AVPixelFormat>(frame->format);
    if (srcFmt == AV_PIX_FMT_NONE) {
        qWarning("player: frame has no pixel format, skip");
        return false;
    }
    int tw = w;
    int th = h;
    computeFrameSize(w, h, &tw, &th);
    if (sws == nullptr || swsW != w || swsH != h || swsFmt != srcFmt ||
        swsTW != tw || swsTH != th) {
        if (sws != nullptr) {
            sws_freeContext(sws);
        }
        // 源 w/h → 目标 tw/th：直接把缩放并进 sws，省掉一次全尺寸中转
        sws = sws_getContext(w, h, srcFmt, tw, th, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                             nullptr, nullptr, nullptr);
        swsW = w;
        swsH = h;
        swsTW = tw;
        swsTH = th;
        swsFmt = srcFmt;
        if (sws == nullptr) {
            qWarning("player: sws_getContext failed(%dx%d->%dx%d fmt=%d)", w, h, tw,
                     th, static_cast<int>(srcFmt));
            return false;
        }
    }
    const int bytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, tw, th, 1);
    if (bytes <= 0) {
        return false;
    }
    rgbBuf.resize(static_cast<std::size_t>(bytes));
    std::uint8_t* dst[4] = {nullptr, nullptr, nullptr, nullptr};
    int dstLines[4] = {0, 0, 0, 0};
    av_image_fill_arrays(dst, dstLines, rgbBuf.data(), AV_PIX_FMT_RGBA, tw, th, 1);
    sws_scale(sws, frame->data, frame->linesize, 0, h, dst, dstLines);

    const QImage image(dst[0], tw, th, dstLines[0], QImage::Format_RGBA8888);
    // 跨线程投递必须自己持有一份像素（rgbBuf 下一帧就被覆盖）
    const QImage owned = image.copy();
    if (owned.isNull()) {
        // 内存真的不够：丢这一帧，绝不让异常/空图往下走
        ++droppedFrames;
        return false;
    }
    frameInFlight = true;
    q->frameReady(owned);
    return true;
}

void PlayerCore::Impl::videoLoop() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (packet == nullptr || frame == nullptr) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        return;
    }
    double lastPts = -1.0;
    double frameDuration = 40.0;
    int flushedGen = seekGen.load();
    int sendFailures = 0;
    while (!quit) {
        if (paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (!videoQ.get(packet, true, 30)) {
            if (readFinished.load() && videoQ.empty()) {
                break;
            }
            continue;
        }
        // 诊断：前几个视频包的位置/大小/是否关键帧。
        // 与 ffprobe -show_packets 的 pos 对照，能直接判定 demux 拿到的码流对不对。
        if (pktLogs < 3) {
            ++pktLogs;
            qWarning("player: vpkt pos=%lld size=%d key=%d",
                     static_cast<long long>(packet->pos), packet->size,
                     (packet->flags & AV_PKT_FLAG_KEY) ? 1 : 0);
        }
        const int packetGen = seekGen.load();
        // seek 后的第一次取包：flush 只能在本线程做（vctx 只归这里）。
        // 在 demux 线程跨线程 flush 会和 send/receive 并发，直接把参考帧状态搅乱。
        if (packetGen != flushedGen) {
            avcodec_flush_buffers(vctx);
            flushedGen = packetGen;
            lastPts = -1.0;
        }
        int sendRc = avcodec_send_packet(vctx, packet);
        if (sendRc == AVERROR(EAGAIN)) {
            // EAGAIN 不是错误：输出队列满，包并未被消费。必须先取走已解出的帧
            // 再重发同一个包。原实现把 EAGAIN 当失败直接丢包 —— 丢一个参考帧
            // 会让后面整段 P/B 帧解不出来（花屏），比少显示一帧严重得多。
            for (;;) {
                const int drainRc = avcodec_receive_frame(vctx, frame);
                if (drainRc < 0) {
                    break;
                }
                av_frame_unref(frame);
            }
            sendRc = avcodec_send_packet(vctx, packet);
        }
        if (sendRc < 0) {
            // 不再静默丢弃：丢一个包就可能让后面整段 P/B 帧参考失效
            if (++sendFailures == 1) {
                qWarning("player: avcodec_send_packet failed (seekGen=%d)", packetGen);
            }
            av_packet_unref(packet);
            continue;
        }
        sendFailures = 0;
        av_packet_unref(packet);
        while (!quit) {
            const int rc = avcodec_receive_frame(vctx, frame);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
                break;
            }
            if (rc < 0) {
                break;
            }
            double ptsMs = 0.0;
            const std::int64_t pts = frame->best_effort_timestamp;
            if (pts != AV_NOPTS_VALUE && vstream != nullptr) {
                ptsMs = static_cast<double>(pts) * av_q2d(vstream->time_base) * 1000.0;
            } else {
                ptsMs = lastPts < 0.0 ? 0.0 : lastPts + frameDuration;
            }
            if (frame->pkt_duration > 0 && vstream != nullptr) {
                frameDuration = frame->pkt_duration * av_q2d(vstream->time_base) * 1000.0;
            }
            // 这个包是 seek 之前入队的：它解出来的帧一律作废（否则向后 seek 会回跳）
            if (seekGen.load() != packetGen) {
                av_frame_unref(frame);
                continue;
            }
            // seek 屏障：目标之前的帧是旧位置残留，直接丢（防止回跳/花屏）
            const qint64 drop = seekDropMs.load();
            if (drop > 0 && ptsMs < static_cast<double>(drop) - kSeekDropSlopMs) {
                av_frame_unref(frame);
                continue;
            }
            if (lastPts >= 0.0 && ptsMs < lastPts - 1.0) {
                lastPts = -1.0;
            }
            // 视频落后音频超过 120ms：丢帧追赶，不做"慢放"（音频是主时钟）
            if (audioDev != 0 && audioClockValid.load() &&
                referenceClockMs() - ptsMs > kVideoDropMs) {
                ++droppedFrames;
                av_frame_unref(frame);
                continue;
            }
            syncToClock(ptsMs);
            // 等时钟的过程中可能又发生了 seek：交付前必须用最新状态再判一次
            if (seekGen.load() != packetGen) {
                av_frame_unref(frame);
                continue;
            }
            const qint64 dropAfterWait = seekDropMs.load();
            if (dropAfterWait > 0 &&
                ptsMs < static_cast<double>(dropAfterWait) - kSeekDropSlopMs) {
                av_frame_unref(frame);
                continue;
            }
            if (audioDev != 0 && audioClockValid.load() &&
                referenceClockMs() - ptsMs > kVideoDropMs) {
                ++droppedFrames;
                av_frame_unref(frame);
                continue;
            }
            if (paused.load() || quit.load()) {
                av_frame_unref(frame);
                continue;
            }
            if (dropAfterWait > 0) {
                seekDropMs = 0;  // 只有真正要渲染 target 之后的帧才解除屏障
            }
            if ((audioDev == 0 || !audioClockValid.load()) && !wallStarted.load()) {
                resetWallClock(ptsMs);  // 无音频：用首个有效帧锚定系统时钟
            }
            // 采样交付瞬间的时钟：deliverFrame 要做 sws+拷贝（毫秒级），期间可能又 seek，
            // 渲染后再读时钟会把"旧帧+新时钟"记成 50s 漂移（测量假象）。
            const qint64 clockAtRender = static_cast<qint64>(referenceClockMs());
            if (seekGen.load() != packetGen) {
                av_frame_unref(frame);  // 最后一道：渲染前再确认没有新 seek
                continue;
            }
            if (deliverFrame(frame)) {
                lastPts = ptsMs;
                positionMs = static_cast<qint64>(ptsMs);
                q->frameSync(static_cast<qint64>(ptsMs), clockAtRender);
                if (!firstFrame.exchange(true)) {
                    q->bufferingChanged(false);
                }
            }
            av_frame_unref(frame);
        }
    }
    videoDone = true;
    av_frame_free(&frame);
    av_packet_free(&packet);
}

int PlayerCore::Impl::decodeAudio(std::uint8_t* out, int maxBytes) {
    if (actx == nullptr || aFrame == nullptr || swr == nullptr) {
        return -1;
    }
    for (;;) {
        if (quit.load()) {
            return -1;
        }
        // 音频解码器同样只在本线程（SDL 回调）flush，避免和 receive 并发
        const int gen = seekGen.load();
        if (gen != audioFlushedGen) {
            avcodec_flush_buffers(actx);
            audioFlushedGen = gen;
        }
        const int rc = avcodec_receive_frame(actx, aFrame);
        if (rc == 0) {
            const std::int64_t pts = aFrame->best_effort_timestamp;
            double framePts = audioClockMs.load();
            if (pts != AV_NOPTS_VALUE && astream != nullptr) {
                framePts = static_cast<double>(pts) * av_q2d(astream->time_base) * 1000.0;
            }
            const qint64 drop = seekDropMs.load();
            if (drop > 0 && framePts < static_cast<double>(drop) - kSeekDropSlopMs) {
                av_frame_unref(aFrame);  // seek 前的音频：丢掉，别让时钟倒退
                continue;
            }
            if (drop > 0) {
                seekDropMs = 0;
            }
            audioFramePts = framePts;
            audioFrameConsumed = 0;
            // 新会话的第一帧（时钟还没生效）直接覆盖时钟：残留的旧位置会让
            // 时钟卡在高位，视频帧被判成「落后」整段丢弃，画面停住不动。
            if (!audioClockValid.load() || framePts > audioClockMs.load()) {
                audioClockMs = framePts;  // 同一会话内保持单调不回退
            }
            audioClockValid = true;
            const int maxSamples = maxBytes / 4;  // S16 立体声
            std::uint8_t* dst[1] = {out};
            const std::uint8_t* inData[AV_NUM_DATA_POINTERS];
            for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i) {
                inData[i] = aFrame->extended_data[i];
            }
            const int got = swr_convert(swr, dst, maxSamples, inData,
                                        aFrame->nb_samples);
            av_frame_unref(aFrame);
            if (got <= 0) {
                return -1;
            }
            return got * 4;
        }
        if (rc != AVERROR(EAGAIN)) {
            return -1;
        }
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            return -1;
        }
        if (!audioQ.get(packet, false)) {
            av_packet_free(&packet);
            return -1;
        }
        int audioSendRc = avcodec_send_packet(actx, packet);
        if (audioSendRc == AVERROR(EAGAIN)) {
            // 与视频侧同源：EAGAIN 表示输入队列满、包未被消费，先取空再重发。
            // 这里取到的帧只能丢弃（本函数每轮只返回一段样本），但丢包会让
            // 解码器状态断掉，后果比丢一帧更重。
            for (;;) {
                const int drainRc = avcodec_receive_frame(actx, aFrame);
                if (drainRc < 0) {
                    break;
                }
                av_frame_unref(aFrame);
            }
            audioSendRc = avcodec_send_packet(actx, packet);
        }
        if (audioSendRc < 0) {
            av_packet_free(&packet);
            continue;
        }
        av_packet_free(&packet);
    }
}

void PlayerCore::Impl::fillAudio(std::uint8_t* stream, int len) {
    std::memset(stream, 0, static_cast<std::size_t>(len));
    if (quit.load() || paused.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(audioMutex);
    int offset = 0;
    while (len > 0) {
        if (audioBufIndex >= audioBufSize) {
            const int got = decodeAudio(audioBuf.data(), static_cast<int>(audioBuf.size()));
            if (got <= 0) {
                audioBufSize = 0;
                audioBufIndex = 0;
                break;
            }
            audioBufSize = got;
            audioBufIndex = 0;
        }
        int chunk = audioBufSize - audioBufIndex;
        if (chunk > len) {
            chunk = len;
        }
        const int vol = volume.load();
        const std::int16_t* src =
            reinterpret_cast<const std::int16_t*>(audioBuf.data() + audioBufIndex);
        std::int16_t* dst = reinterpret_cast<std::int16_t*>(stream + offset);
        const int samples = chunk / 2;
        for (int i = 0; i < samples; ++i) {
            dst[i] = static_cast<std::int16_t>((static_cast<int>(src[i]) * vol) / 100);
        }
        audioBufIndex += chunk;
        offset += chunk;
        len -= chunk;
        // 音频时钟 = 当前帧起点 + 已交给声卡的样本数/采样率（单调递增）
        int consumed = audioFrameConsumed.load() + samples;
        audioFrameConsumed = consumed;
        const double played =
            audioFramePts.load() +
            static_cast<double>(consumed) * 1000.0 / kAudioSampleRate;
        if (played > audioClockMs.load()) {
            audioClockMs = played;
        }
    }
}

void PlayerCore::Impl::sdlAudioCallback(void* userdata, std::uint8_t* stream, int len) {
    static_cast<Impl*>(userdata)->fillAudio(stream, len);
}

qint64 PlayerCore::Impl::currentPositionMs() {
    const qint64 pos = positionMs.load();
    if (durationMs.load() > 0 && pos > durationMs.load()) {
        return durationMs.load();
    }
    return pos < 0 ? 0 : pos;
}

// -------------------------------------------------------------------- PlayerCore

PlayerCore::PlayerCore(QObject* parent) : QObject(parent), m_impl(new Impl(this)) {
    m_impl->audioBuf.resize(kAudioBufferBytes);
    m_impl->tick = new QTimer(this);
    m_impl->tick->setInterval(kTickMs);
    connect(m_impl->tick, &QTimer::timeout, this, [this]() {
        Impl* d = m_impl;
        if (d->fmt == nullptr) {
            return;
        }
        emit positionChanged(d->currentPositionMs());
        // 首帧到了就从"缓冲中"切到"播放中"（契约 §3）
        if (d->firstFrame.load() && !d->paused.load() && d->lastState == Buffering) {
            d->setState(Playing);
        }
        if (d->videoDone.load() && d->readFinished.load() && !d->endedEmitted.load()) {
            d->endedEmitted = true;
            d->tick->stop();
            d->setState(Ended);
            emit ended();
        }
    });
}

PlayerCore::~PlayerCore() {
    close();
    delete m_impl;
}

void PlayerCore::open(const QString& path, bool progressive, qint64 totalBytes) {
    // 实例级追踪：日志里的两个 h264 上下文要么来自两次 open，要么来自两个 PlayerCore。
    qWarning("player: open(%p) progressive=%d size=%lld %s",
             static_cast<void*>(this), progressive ? 1 : 0,
             static_cast<long long>(totalBytes), qPrintable(path));
    close();
    m_impl->expectedSize = totalBytes > 0 ? totalBytes : 0;
    m_impl->setState(Opening);
    QString err;
    if (!m_impl->openMedia(path, progressive, &err)) {
        m_impl->errorText = err;
        // openMedia 中途失败时 fmt/io/vctx 可能已经分配了一半，必须回收：
        // 否则下次 open 面对的是「半初始化」的上下文，切换文件时行为不可预期。
        m_impl->closeMedia();
        emit errorChanged(err);
        m_impl->setState(Error);
        return;
    }
    m_impl->setState(Buffering);
    emit bufferingChanged(true);
    m_impl->tick->start();
}

void PlayerCore::setReadableBytes(qint64 bytes) {
    if (m_impl != nullptr && m_impl->io != nullptr) {
        m_impl->io->commitReadable(bytes);
    }
}

void PlayerCore::close() {
    const bool wasOpen = m_impl->fmt != nullptr;
    m_impl->tick->stop();
    m_impl->closeMedia();
    if (wasOpen) {
        m_impl->setState(Idle);
    }
}

void PlayerCore::play() {
    if (m_impl->fmt == nullptr) {
        return;
    }
    m_impl->paused = false;
    m_impl->resetWallClock(static_cast<double>(m_impl->positionMs.load()));
    if (m_impl->audioDev != 0) {
        SDL_PauseAudioDevice(m_impl->audioDev, 0);
    }
    m_impl->setState(Playing);
}

void PlayerCore::pause() {
    if (m_impl->fmt == nullptr) {
        return;
    }
    m_impl->paused = true;
    if (m_impl->audioDev != 0) {
        SDL_PauseAudioDevice(m_impl->audioDev, 1);
    }
    m_impl->setState(Paused);
}

void PlayerCore::togglePlay() {
    if (m_impl->paused.load()) {
        play();
    } else {
        pause();
    }
}

void PlayerCore::seek(qint64 ms) {
    if (m_impl->fmt == nullptr) {
        return;
    }
    if (ms < 0) {
        ms = 0;
    }
    if (m_impl->durationMs.load() > 0 && ms > m_impl->durationMs.load()) {
        ms = m_impl->durationMs.load();
    }
    m_impl->seekTargetMs = ms;
    m_impl->seekReq = true;
    m_impl->positionMs = ms;  // 只让 UI 立刻跟手；时钟基准必须等 demux 线程 flush 后
                              // 在 doSeek 里重置，否则旧帧会和新时钟打架（漂移 50s）
}

void PlayerCore::setVolume(int percent) {
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    m_impl->volume = percent;
}

void PlayerCore::setSourceComplete(bool complete) {
    if (m_impl->io != nullptr) {
        m_impl->io->setSourceComplete(complete);
    }
}

void PlayerCore::setDisplaySize(int pixelWidth, int pixelHeight) {
    m_impl->dispW = pixelWidth > 0 ? pixelWidth : 0;
    m_impl->dispH = pixelHeight > 0 ? pixelHeight : 0;
}

void PlayerCore::notifyFrameConsumed() { m_impl->frameInFlight = false; }

PlayerCore::State PlayerCore::state() const {
    return static_cast<State>(m_impl->lastState);
}

qint64 PlayerCore::position() const { return m_impl->currentPositionMs(); }

qint64 PlayerCore::duration() const { return m_impl->durationMs.load(); }

int PlayerCore::videoWidth() const { return m_impl->vctx != nullptr ? m_impl->vctx->width : 0; }

int PlayerCore::videoHeight() const { return m_impl->vctx != nullptr ? m_impl->vctx->height : 0; }

bool PlayerCore::isOpen() const { return m_impl->fmt != nullptr; }

bool PlayerCore::isPaused() const { return m_impl->paused.load(); }

QString PlayerCore::path() const { return m_impl->path; }

QString PlayerCore::errorText() const { return m_impl->errorText; }

qint64 PlayerCore::availableBytes() const {
    return m_impl->io != nullptr ? m_impl->io->availableBytes() : 0;
}

qint64 PlayerCore::audioClockMs() const {
    return static_cast<qint64>(m_impl->audioClockMs.load());
}

int PlayerCore::droppedFrames() const { return m_impl->droppedFrames.load(); }

bool PlayerCore::hasAudio() const {
    return m_impl->audioIndex >= 0 && m_impl->audioDev != 0;
}

}  // namespace player
}  // namespace cm
