#include "playerdialog.h"

#include <QApplication>
#include <QFile>
#include <QDebug>


#include<iostream>
using namespace std;

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

//把 qDebug 日志写入文件, 方便排查问题
static void fileMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    static QFile file("D:/mediaplayer/player_debug.log");
    if( !file.isOpen() ) file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    if( file.isOpen() )
    {
        QString text;
        switch( type )
        {
        case QtDebugMsg: text = "Debug: "; break;
        case QtWarningMsg: text = "Warning: "; break;
        case QtCriticalMsg: text = "Critical: "; break;
        case QtFatalMsg: text = "Fatal: "; break;
        default: text = "Info: "; break;
        }
        text += msg + "\n";
        file.write( text.toLocal8Bit() );
        file.flush();
    }
}

#undef main
int main(int argc, char *argv[])
{
    qInstallMessageHandler( fileMessageHandler );
    QApplication a(argc, argv);


    //这里简单的输出一个版本号
    cout << "Hello FFmpeg!" << endl;
    av_register_all();
    unsigned version = avcodec_version();//编解码器
    cout << "version is:" << version<<endl;

    PlayerDialog w;
    w.show();

    //支持命令行参数直接播放: MediaPlayer.exe <文件路径>
    if( a.arguments().size() > 1 )
    {
        w.openAndPlay( a.arguments().at(1) );
    }

    return a.exec();
}
