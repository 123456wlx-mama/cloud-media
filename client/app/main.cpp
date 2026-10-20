// cloud-media 客户端入口。
// P8-2：真实上传/下载 + 断点续传；P8-3：分享、回收站、秒传、目录分页。
// 网络在 netapi 线程，业务状态机在 shareapi/recycleapi/transfer，任务落 SQLite，
// UI 只读状态并转发动作（契约 §1.3）。
//
// 用法：
//   cloud-media-client --version
//   cloud-media-client --host 192.168.142.131 --port 8004 --tel 139... --password-env CM_CLI_PASSWORD
//   cloud-media-client ... --smoke [--upload F | --download UF | --share-create UF |
//                                --share-open CODE --access 1234 | --delete UF |
//                                --cd UF | --page-size 50]
#include <QApplication>
#include <QFile>
#include <QFont>
#include <QString>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "app/smoke.h"
#include "cm/protocol/version.h"
#include "cm/version.h"
#include "netapi/NetClient.h"
#include "recycleapi/RecycleFlow.h"
#include "shareapi/ShareFlow.h"
#include "sqlapi/TaskStore.h"
#include "transfer/TransferQueue.h"
#include "ui/LoginDialog.h"
#include "ui/MainWindow.h"

namespace {

struct Options {
    QString host;
    quint16 port;
    QString tel;
    QString password;
    cm::app::SmokeOptions smoke;
    bool demo;
    bool smokeMode;
    QString screenshot;

    Options() : host(QString::fromUtf8("127.0.0.1")), port(8004),
                demo(false), smokeMode(false) {}
};

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
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--version") {
            std::cout << "cloud-media-client " << cm::version()
                      << " (protocol v" << cm::protocol::version() << ")\n";
            return 0;
        }
        if (arg == "--host" && i + 1 < argc) {
            opt.host = QString::fromStdString(argv[++i]);
        } else if (arg == "--port" && i + 1 < argc) {
            opt.port = static_cast<quint16>(std::atoi(argv[++i]));
        } else if (arg == "--tel" && i + 1 < argc) {
            opt.tel = QString::fromStdString(argv[++i]);
        } else if (arg == "--password" && i + 1 < argc) {
            opt.password = QString::fromStdString(argv[++i]);
        } else if (arg == "--password-env" && i + 1 < argc) {
            const char* value = std::getenv(argv[++i]);
            if (value != nullptr) {
                opt.password = QString::fromUtf8(value);
            }
        } else if (arg == "--upload" && i + 1 < argc) {
            opt.smoke.uploadPath = QString::fromStdString(argv[++i]);
        } else if (arg == "--download" && i + 1 < argc) {
            opt.smoke.downloadUfId = QString::fromStdString(argv[++i]);
        } else if (arg == "--share-create" && i + 1 < argc) {
            opt.smoke.shareCreateUfId = QString::fromStdString(argv[++i]);
        } else if (arg == "--share-open" && i + 1 < argc) {
            opt.smoke.shareOpenCode = QString::fromStdString(argv[++i]);
        } else if (arg == "--share-cancel" && i + 1 < argc) {
            opt.smoke.shareCancelCode = QString::fromStdString(argv[++i]);
        } else if (arg == "--access" && i + 1 < argc) {
            opt.smoke.accessCode = QString::fromStdString(argv[++i]);
        } else if (arg == "--delete" && i + 1 < argc) {
            opt.smoke.deleteUfId = QString::fromStdString(argv[++i]);
        } else if (arg == "--purge" && i + 1 < argc) {
            opt.smoke.purgeUfId = QString::fromStdString(argv[++i]);
        } else if (arg == "--cd" && i + 1 < argc) {
            opt.smoke.cdUfId = QString::fromStdString(argv[++i]);
        } else if (arg == "--page-size" && i + 1 < argc) {
            opt.smoke.pageLimit = static_cast<quint32>(std::atoi(argv[++i]));
        } else if (arg == "--ui-demo") {
            opt.demo = true;
        } else if (arg == "--smoke") {
            opt.smokeMode = true;
        } else if (arg == "--auto-resume") {
            opt.smoke.autoResume = true;
        } else if (arg == "--screenshot" && i + 1 < argc) {
            opt.screenshot = QString::fromStdString(argv[++i]);
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName(QString::fromUtf8("cloud-media"));
    applyTheme(&app);

    cm::sqlapi::TaskStore store;
    store.open(QString::fromUtf8("cm-tasks.db"));

    cm::netapi::NetClient net;
    cm::transfer::TransferQueue queue(&net, &store);
    queue.restoreFromStore();
    net.connectTo(opt.host, opt.port);

    if (opt.smokeMode) {
        if (opt.tel.isEmpty() || opt.password.isEmpty()) {
            std::printf("SMOKE_NEED_CREDENTIALS\n");
            return 1;
        }
        cm::shareapi::ShareFlow share(&net);
        cm::recycleapi::RecycleFlow recycle(&net);
        cm::app::runSmoke(&net, &queue, &share, &recycle, opt.smoke);
        net.login(opt.tel, opt.password);
        return app.exec();
    }

    if (opt.demo) {
        cm::ui::MainWindow window(&net, &queue);
        window.show();
        if (!opt.screenshot.isEmpty()) {
            const QString path = opt.screenshot;
            QTimer::singleShot(600, [&window, path]() {
                window.grab().save(path);
                QApplication::quit();
            });
        }
        return app.exec();
    }

    const bool autoLogin = !opt.tel.isEmpty() && !opt.password.isEmpty();
    if (!autoLogin) {
        cm::ui::LoginDialog login(&net);
        if (login.exec() != QDialog::Accepted) {
            return 0;
        }
        cm::ui::MainWindow window(&net, &queue);
        window.show();
        return app.exec();
    }

    cm::ui::MainWindow window(&net, &queue);
    window.show();
    net.login(opt.tel, opt.password);
    if (!opt.screenshot.isEmpty()) {
        const QString path = opt.screenshot;
        QTimer::singleShot(2500, [&window, path]() {
            window.grab().save(path);
            QApplication::quit();
        });
    }
    return app.exec();
}
