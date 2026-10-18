#ifndef COMMON_H
#define COMMON_H
#include<QString>
//文件信息：
struct FileInfo
{

    FileInfo():fileid(0) , size(0),pFile( nullptr )
      , pos(0) , isPause(0),timestamp(0){

    }

    int fileid;
    QString name;
    QString dir;//网盘目录
    QString time;
    int size;//int 32位 有符号最大2个GB----假定网盘最大文件是2GB最大
    QString md5;
    QString type;
    QString absolutePath;//文件的绝对路径

    int pos; //上传或下载到什么位置
    int timestamp;//时间戳 文件身份
    int isPause; //暂停  0 1

    //文件指针
    FILE* pFile;

//字节单位换算，输入字节size，输出带单位字符串 B / KB / MB / GB
static QString getSize(int size)
{
    QString res;
    int count = 0;
    int tmp = size;
    while( tmp != 0 )
    {
        tmp /= 1024;
        if( tmp != 0 )
            count++;
    }

    switch( count )
    {
    case 0: // 小于1024，单位 B（字节）
        res = QString("%1.%2B").arg(0)
                .arg( (int)(size%1024/1024.0*100), 2, 10, QChar('0') );
        if(size!=0&&res=="0.00KB")
        {
            res="0.01KB";
        }
        break;
    case 1: // 1024 ~ 1024*1024-1，单位 KB
        res = QString("%1.%2KB").arg( size/1024 )
                .arg( (int)( size%1024/1024.0*100 ), 2, 10, QChar('0') );
        break;
    case 2: // 1024*1024 ~ 1024^3-1，单位 MB
    case 3: // 1024^3 ~ 1024^4-1，单位 GB
        res = QString("%1.%2MB").arg( size/1024/1024 )
                .arg( (int)( (size/1024)%1024 /1024.0*100 ), 2, 10, QChar('0') );
        break;
    default:
        break;
    }
    return res;
}

};

#endif // COMMON_H
