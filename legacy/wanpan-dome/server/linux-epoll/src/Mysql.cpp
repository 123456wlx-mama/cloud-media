#include "Mysql.h"



int CMysql::ConnectMysql(const char* server,const char* user,const char* password, const char* database)
{
    conn = mysql_init(NULL);  // 创建MYSQL对象。NULL表示加载默认值。
    mysql_set_character_set(conn,"utf8");//设置字符集为UTF8
    if(!mysql_real_connect(conn,server,user,password,database,0,NULL,0))//连接数据库
    {
        return FALSE;// 连接失败，返回FALSE
    }
    pthread_mutex_init(&m_lock , NULL); // 初始化互斥锁

    return TRUE;  // 连接成功，返回TRUE
}

int CMysql::SelectMysql(char* szSql,int nColumn,list<string>& lst)
{
    MYSQL_RES * results = NULL;

    pthread_mutex_lock(&m_lock ); // 加锁

    if(mysql_query(conn,szSql)) {  // 查询SQL是否合法
        pthread_mutex_unlock(&m_lock );  // 查询失败，解锁并返回FALSE
        return FALSE;  
    }
    results = mysql_store_result(conn);  // 将结果放在result中
    pthread_mutex_unlock(&m_lock );   // 解锁

    if(NULL == results)return FALSE;   // result为空，返回FALSE
    MYSQL_ROW record;     // 定义记录行
    while((record = mysql_fetch_row(results)))  // 遍历查询结果
    {
        for(int i=0; i<nColumn; i++)   
        {
            lst.push_back( record[i] );  // 将查询结果存入list
        }
    }
    return TRUE;  // 查询成功，返回TRUE
}


int CMysql::UpdataMysql(char *szsql)
{
    if(!szsql)return FALSE;   // SQL为空，返回FALSE
    pthread_mutex_lock(&m_lock );   // 加锁
    if(mysql_query(conn,szsql)){    // 执行SQL语句
        pthread_mutex_unlock(&m_lock );  // 执行失败，解锁并返回FALSE
        return FALSE;
    }
    pthread_mutex_unlock(&m_lock );  // 解锁
    return TRUE;   // SQL执行成功，返回TRUE
}

void CMysql::DisConnect()
{
    mysql_close(conn);  // 关闭数据库连接
}
