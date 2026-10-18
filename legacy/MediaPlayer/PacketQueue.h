#ifndef PACKETQUEUE_H
#define PACKETQUEUE_H

#ifdef __cplusplus
extern "C"{
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <SDL.h>

#ifdef __cplusplus
}
#endif

typedef struct PacketQueue{
    AVPacketList *first_pkt; // 队头
    AVPacketList *last_pkt;  // 队尾
    int nb_packets;          // 包数量
    int size;                // 队列总字节大小
    SDL_mutex *mutex;        // 互斥锁
    SDL_cond *cond;          // 条件变量
    int quit;                // 1: 退出标志, 阻塞中的 packet_queue_get 返回 0
}PacketQueue;

// 初始化数据包队列
void packet_queue_init(PacketQueue *queue);
// 数据包入队
int packet_queue_put(PacketQueue *queue, AVPacket *packet);
// 数据包出队
int packet_queue_get(PacketQueue *queue, AVPacket *packet, int block);
// 唤醒并结束阻塞中的 packet_queue_get(用于播放结束/停止时退出解码线程)
void packet_queue_abort(PacketQueue *queue);
// 清空队列中剩余的包(跳转seek时使用), 队列仍可继续使用
void packet_queue_flush(PacketQueue *queue);
// 清空队列: 释放剩余包 + 销毁锁
void packet_queue_destroy(PacketQueue *queue);

#endif // PACKETQUEUE_H
