// cloud-media 客户端入口。
// Phase 8 骨架：登录对话框 → 三段式主窗口；协议层尚未接线（P8-1 起做网络线程）。
//
// 用法：
//   cloud-media-client --version                       # 只打印版本，不创建 GUI
//   cloud-media-client --ui-demo                       # 跳过登录直接展示主窗口
//   cloud-media-client --ui-demo --screenshot out.png   # 渲染主窗口到 PNG 后退出（CI 用）
#include <QApplication>
#include <QFile>
#include <QFont>
#include <QString>
#include <QTimer>

#include <iostream>
#include <string>

#include "cm/protocol/version.h"
#include "cm/version.h"
#include "ui/LoginDialog.h"
#include "ui/MainWindow.h"

namespace {

// 深色主题基线：色板与字号见 docs/specs/08-界面设计契约.md §4。
void applyTheme(QApplication* app) {
    QFont font(QString::fromUtf8("Microsoft YaHei UI"), 9);
    app->setFont(font);

    QFile qss(QString::fromUtf8(":/app.qss"));
    if (qss.open(QIODevice::ReadOnly)) {
        app->setStyleSheet(QString::fromUtf8(qss.readAll()));
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool demo = false;
    std::string screenshot;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--version") {
            std::cout << "cloud-media-client " << cm::version()
                      << " (protocol v" << cm::protocol::version() << ")\n";
            return 0;
        }
        if (arg == "--ui-demo") {
            demo = true;
        }
        if (arg == "--screenshot" && i + 1 < argc) {
            screenshot = argv[++i];
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName(QString::fromUtf8("cloud-media"));
    applyTheme(&app);

    if (demo) {
        cm::ui::MainWindow window;
        window.show();
        if (!screenshot.empty()) {
            const QString path = QString::fromStdString(screenshot);
            // 截图模式：等一帧布局/QSS 生效后 grab 再退出，无需真实桌面会话。
            QTimer::singleShot(600, [&window, path]() {
                window.grab().save(path);
                QApplication::quit();
            });
        }
        return app.exec();
    }

    cm::ui::LoginDialog login;
    if (login.exec() != QDialog::Accepted) {
        return 0;
    }
    cm::ui::MainWindow window;
    window.show();
    return app.exec();
}
