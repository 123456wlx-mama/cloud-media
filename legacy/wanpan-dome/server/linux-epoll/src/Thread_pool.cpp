#include <Thread_pool.h>
#include "packdef.h"

/* 自定义线程池结构体 */
STRU_POOL_T::STRU_POOL_T(int max, int min, int que_max)
{
    //初始化标志
	this->thread_max = max;  		// 最大线程数
	this->thread_min = min;  		// 最小线程数
	this->thread_alive = 0;  		// 存活的线程数(初始为 0)
	this->thread_busy = 0;   		// 忙碌的线程数(初始为 0)
	this->thread_shutdown = TRUE;  	// 线程池的关闭状态(初始为 TRUE)
	this->thread_wait = 0;   		// 需要等待关闭的线程数(初始为 0)
	this->queue_max = que_max;  	// 任务队列所能容纳的最大任务数
	this->queue_cur = 0;     		// 当前任务队列中的任务数(初始为 0)
	this->queue_front = 0;   		// 队列头指针(初始为 0)
	this->queue_rear = 0;    		// 队列尾指针(初始为 0)

    //初始化互斥锁 条件变量
    if(pthread_cond_init(&this->not_full,NULL)!=0 ||
            pthread_cond_init(&this->not_empty,NULL)!=0 ||
            pthread_mutex_init(&this->lock,NULL)!=0)
    {
        err_str("init cond or mutex error:",-1); // 错误处理
    }
    //初始化线程数组
    if((this->tids = (pthread_t*)malloc(sizeof(pthread_t)*max)) == NULL)
    {
        err_str("malloc tids error:",-1);// 错误处理
    }
    bzero(this->tids,sizeof(pthread_t)*max); // 清空线程数组
    //初始化任务队列
    if((this->queue_task = (task_t*)malloc(sizeof(task_t)*que_max))==NULL)
    {
        err_str("malloc task queue error:",-1);// 错误处理
    }

}


/* 检查线程是否存活 */
int thread_pool::if_thread_alive(pthread_t tid)
{
    if((pthread_kill(tid,0)) == -1)
    {
        if(errno == ESRCH) // 线程不存在
            return FALSE;
    }

    return TRUE;
}

/* 创建线程池 */
bool thread_pool::Pool_create(int max,int min,int que_max)
{
    m_pool = new STRU_POOL_T( max , min , que_max );// 创建线程池对象

    int err = 0;
    for(int i=0; i< min; i++) // 创建最小数量的工作线程
    {
        if((err = pthread_create(&m_pool->tids[i],NULL,  Custom, (void*)m_pool))>0)
        {
            printf("create custom error:%s\n",strerror(err));// 错误处理
            return false;
        }
        ++(m_pool->thread_alive);// 存活的线程数加一
    }

    if((err = pthread_create(&(m_pool->manager_tid),NULL, Manager,(void*)m_pool))>0)// 创建管理线程
    {
        printf("create custom error:%s\n",strerror(err));// 错误处理
        return false;
    }
    return true;
}


/* 添加任务 生产者*/
int thread_pool::Producer_add( void *(*task)(void *arg),void *arg)
{
    pthread_mutex_lock(&m_pool->lock);// 上锁
	// 当前任务队列已满，且线程池未关闭
    while(m_pool->queue_cur == m_pool->queue_max && m_pool->thread_shutdown  )
    {
		// 队列已满，等待有任务被取出
        pthread_cond_wait(&m_pool->not_full,&m_pool->lock);
    }
	// 线程池已关闭
    if(!m_pool->thread_shutdown  )
    {
        pthread_mutex_unlock(&m_pool->lock); 
        return -1; //退出
    }
	// 将任务加入任务队列
    m_pool->queue_task[m_pool->queue_front].task = task;
    m_pool->queue_task[m_pool->queue_front].arg = arg;
	// 更新队列头指针
    m_pool->queue_front = (m_pool->queue_front + 1) % m_pool->queue_max;
	// 当前任务队列中的任务数加一
    ++(m_pool->queue_cur);
	// 通知消费者线程有新的任务可取
    pthread_cond_signal(&m_pool->not_empty);
	
    pthread_mutex_unlock(&m_pool->lock); // 解锁
    return 0;
}

/* 工作线程函数 消费者*/
void * thread_pool::Custom(void * arg)
{
	// 获取线程池对象指针
    pool_t * p = (pool_t*)arg;
    task_t task;
    while(p->thread_shutdown)  // 线程池未关闭
    {
        pthread_mutex_lock(&p->lock);// 上锁
		// 当前任务队列中没有任务，且线程池未关闭
        while(p->queue_cur == 0 && p->thread_shutdown  )
        {
			// 等待有新的任务被添加到队列中
            pthread_cond_wait(&p->not_empty,&p->lock);
        }
		// 线程池已关闭
        if(!p->thread_shutdown  )
        {
            pthread_mutex_unlock(&p->lock);
            pthread_exit(NULL); // 结束此线程
        }
		// 需要等待关闭的线程数大于零，且当前存活的线程数大于最小线程数
        if(p->thread_wait > 0 && p->thread_alive > p->thread_min)
        {
            --(p->thread_wait); // 等待关闭的线程数减一
            --(p->thread_alive);// 存活的线程数减一
            pthread_mutex_unlock(&p->lock);// 解锁
            pthread_exit(NULL);// 结束此线程
        }
		// 取出任务
        task.task = p->queue_task[p->queue_rear].task;
        task.arg = p->queue_task[p->queue_rear].arg;
        p->queue_rear = (p->queue_rear + 1) % p->queue_max;// 更新队列尾指针
        --(p->queue_cur);// 当前任务队列中的任务数减一
        pthread_cond_signal(&p->not_full); // 通知生产者线程有新的任务可供添加
        ++(p->thread_busy);// 忙碌的线程数加一
        pthread_mutex_unlock(&p->lock);// 解锁
        //执行核心工作
        (*task.task)(task.arg);
        pthread_mutex_lock(&p->lock);// 上锁
        --(p->thread_busy);// 忙碌的线程数减一
        pthread_mutex_unlock(&p->lock); // 解锁
    }
    return 0;
}

/* 线程池管理线程函数 */
void *thread_pool::Manager(void *arg)
{
    pool_t * p = (pool_t *)arg;// 获取线程池对象指针
    int alive = 0;
    int cur = 0;
    int busy = 0;
    int add = 0;
    while(p->thread_shutdown ) // 线程池未关闭
    {
        pthread_mutex_lock(&p->lock); // 上锁
        alive = p->thread_alive; // 存活的线程数
        busy = p->thread_busy;  // 忙碌的线程数
        cur = p->queue_cur; // 当前任务队列中的任务数
        pthread_mutex_unlock(&p->lock); // 解锁
		/* 当前任务队列中的任务数大于存活的空闲线程数，或忙碌线程占比大于等于 80%，且存活的线程数小于最大线程数 */
        if((cur > alive - busy || (float)busy / alive*100 >= (float)80 ) &&
                p->thread_max > alive)
        {
            for(int j = 0;j<p->thread_min;j++)
            {
                for(int i = 0;i<p->thread_max;i++)
                {
					// 该线程不存在或已经结束
                    if(p->tids[i] == 0 || !if_thread_alive(p->tids[i]))
                    {
                        pthread_mutex_lock(&p->lock);// 上锁
						// 创建新的工作线程
                        pthread_create(&p->tids[i],NULL,Custom,(void*)p);
						// 存活的线程数加一
                        ++(p->thread_alive);
                        pthread_mutex_unlock(&p->lock);// 解锁
                        break;
                    }
                }
            }
        }
		/* 忙碌的线程数小于存活线程数的1/3，且存活的线程数大于最小线程数*/
        if(busy *2 < alive - busy && alive > p->thread_min)
        {
            pthread_mutex_lock(&p->lock);// 上锁
            p->thread_wait = _DEF_COUNT;// 设置需要等待关闭的线程数为默认值
            pthread_mutex_unlock(&p->lock);// 解锁
            for(int i=0; i<_DEF_COUNT; i++)
            {
                pthread_cond_signal(&p->not_empty);// 通知工作线程有新的任务可供执行
            }
        }
        sleep(_DEF_TIMEOUT);// 线程挂起一段时间，节省 CPU 资源
    }
    return 0;
}




