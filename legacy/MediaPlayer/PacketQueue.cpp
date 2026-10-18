#include "PacketQueue.h"

// 初始化队列
void packet_queue_init(PacketQueue *queue)
{
    queue->first_pkt = NULL;
    queue->last_pkt = NULL;
    queue->nb_packets = 0;
    queue->size = 0;
    queue->quit = 0;
    queue->mutex = SDL_CreateMutex();
    queue->cond = SDL_CreateCond();
}

// 入队 -- 尾部添加
int packet_queue_put(PacketQueue *queue, AVPacket *packet)
{
    AVPacketList *pkt_list;
    pkt_list = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if (pkt_list == NULL)
    {
        return -1;
    }
    // 确保 packet 的数据是引用计数的(av_read_frame 出来的包本来就是,
    // 这里再兜一层, av_dup_packet 已废弃, 用 av_packet_make_refcounted 替代)
    if (av_packet_make_refcounted(packet) < 0)
    {
        av_free(pkt_list);
        return -1;
    }
    pkt_list->pkt = *packet;
    pkt_list->next = NULL;

    // 上锁
    SDL_LockMutex(queue->mutex);
    if (queue->last_pkt == NULL) // 空队列
    {
        queue->first_pkt = pkt_list;
    }
    else
    {
        queue->last_pkt->next = pkt_list;
    }
    queue->last_pkt = pkt_list;
    queue->nb_packets++;
    queue->size += packet->size;
    SDL_CondSignal(queue->cond); // 唤醒等待的消费者
    SDL_UnlockMutex(queue->mutex);
    return 0;
}

/// 出队--头部删除
/// queue：队列指针
/// pkt：输出参数，取出的packet
/// block：是否阻塞，1队空阻塞等待，0不阻塞直接返回
int packet_queue_get(PacketQueue *queue, AVPacket *packet, int block)
{
    AVPacketList *pkt_list = NULL;
    int ret = 0;
    SDL_LockMutex(queue->mutex);
    while(1)
    {
        pkt_list = queue->first_pkt;
        if (pkt_list != NULL) // 队列不为空
        {
            queue->first_pkt = queue->first_pkt->next;
            if (queue->first_pkt == NULL)
            {
                queue->last_pkt = NULL;
            }
            queue->nb_packets--;
            queue->size -= pkt_list->pkt.size;
            *packet = pkt_list->pkt; // 拷贝packet到输出参数
            av_free(pkt_list);
            ret = 1;
            break;
        }
        else if (block == 0 || queue->quit) // 非阻塞模式/已经退出，队空直接返回
        {
            ret = 0;
            break;
        }
        else // 阻塞模式，等待条件变量
        {
            SDL_CondWait(queue->cond, queue->mutex);
        }
    }
    SDL_UnlockMutex(queue->mutex);
    return ret;
}

// 通知阻塞中的 packet_queue_get 退出
void packet_queue_abort(PacketQueue *queue)
{
    if( !queue ) return;
    SDL_LockMutex(queue->mutex);
    queue->quit = 1;
    SDL_CondBroadcast(queue->cond); // 广播, 唤醒所有等待者
    SDL_UnlockMutex(queue->mutex);
}

// 清空队列中剩余的包(跳转seek时使用), 队列仍可继续使用
void packet_queue_flush(PacketQueue *queue)
{
    AVPacketList *pkt_list;
    if( !queue ) return;
    SDL_LockMutex( queue->mutex );
    pkt_list = queue->first_pkt;
    while( pkt_list )
    {
        AVPacketList *next = pkt_list->next;
        av_packet_unref( &pkt_list->pkt );
        av_free( pkt_list );
        pkt_list = next;
    }
    queue->first_pkt = NULL;
    queue->last_pkt = NULL;
    queue->nb_packets = 0;
    queue->size = 0;
    SDL_UnlockMutex( queue->mutex );
}

// 清空队列并销毁锁(销毁之后队列不可再用)
void packet_queue_destroy(PacketQueue *queue)
{
    AVPacketList *pkt_list;
    if( !queue ) return;
    packet_queue_abort( queue );

    SDL_LockMutex(queue->mutex);
    pkt_list = queue->first_pkt;
    while( pkt_list )
    {
        AVPacketList *next = pkt_list->next;
        av_packet_unref( &pkt_list->pkt );
        av_free( pkt_list );
        pkt_list = next;
    }
    queue->first_pkt = NULL;
    queue->last_pkt = NULL;
    queue->nb_packets = 0;
    queue->size = 0;
    SDL_UnlockMutex(queue->mutex);

    if( queue->cond )  { SDL_DestroyCond( queue->cond );  queue->cond = NULL; }
    if( queue->mutex ) { SDL_DestroyMutex( queue->mutex ); queue->mutex = NULL; }
}
