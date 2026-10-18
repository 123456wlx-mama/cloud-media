#include <QApplication>
#include "ckernel.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    // 获取单例核心类，构造时自动创建窗口、加载配置
    CKernel::GetInstance();
    // 启动Qt事件循环
    return a.exec();
}
