// cloud-media 客户端入口。
// P8-2：真实上传/下载 + 断点续传。网络在 netapi 线程，传输状态机在 transfer，
// 任务落 SQLite（sqlapi），UI 只读状态。
//
// 用法：
//   cloud-media-client --version
//   cloud-media-client --host 192.168.142.131 --port 8004 --tel 139... --password-env CM_CLI_PASSWORD
//   cloud-media-client ... --smoke --upload <local> --download <ufId>   # 无界面联调取证
#include <QApplication>
#include <QFile>
#include <QFont>
#include <QFileInfo>
#include <QHash>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "cm/protocol/version.h"
#include "cm/version.h"
#include "netapi/NetClient.h"
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
    QString uploadPath;
    QString downloadUfId;
    bool demo;
    bool smoke;
    bool autoResume;
    QString screenshot;

    Options() : host(QString::fromUtf8("127.0.0.1")), port(8004), demo(false),
                smoke(false), autoResume(false) {}
};

void applyTheme(QApplication* app) {
    QFont font(QString::fromUtf8("Microsoft YaHei UI"), 9);
    app->setFont(font);
    QFile qss(QString::fromUtf8(":/app.qss"));
    if (qss.open(QIODevice::ReadOnly)) {
        app->setStyleSheet(QString::fromUtf8(qss.readAll()));
    }
}

const char* utf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return bytes.constData();
}

// 无界面联调：登录 → 列表 → （可选）上传 → （可选）下载 → 打印 → 退出。
int runSmoke(cm::netapi::NetClient* net, cm::transfer::TransferQueue* queue,
             const Options& opt) {
    QObject::connect(net, &cm::netapi::NetClient::linkState,
                     [](int state, const QString& detail) {
        std::printf("LINK state=%d detail=%s\n", state, utf8(detail));
        std::fflush(stdout);
    });
    QObject::connect(net, &cm::netapi::NetClient::loginFinished,
                     [net, queue, opt](quint32 code, quint64 uid, const QString& name) {
        if (code != 0) {
            std::printf("LOGIN_FAIL code=%u\n", code);
            std::fflush(stdout);
            QCoreApplication::exit(2);
            return;
        }
        std::printf("LOGIN_OK uid=%llu name=%s\n",
                    static_cast<unsigned long long>(uid), utf8(name));
        std::fflush(stdout);
        // 重连后自动重登成功，才把断线暂停的任务续上（此时会话已有效）。
        if (opt.autoResume && queue != nullptr) {
            const QVector<cm::transfer::TransferTask> tasks = queue->tasks();
            for (int i = 0; i < tasks.size(); ++i) {
                const cm::transfer::TransferTask task = tasks.at(i);
                if (task.state != cm::transfer::TaskState::Paused) {
                    continue;
                }
                std::printf("RESUME task=%d bytesDone=%llu blocks=%u/%u\n", task.id,
                            static_cast<unsigned long long>(task.bytesDone),
                            cm::transfer::bitmapCount(task.bitmap, task.blockCount),
                            task.blockCount);
                std::fflush(stdout);
                queue->resume(task.id);
            }
        }
        net->listFiles(0, 50);
    });
    QObject::connect(net, &cm::netapi::NetClient::listFinished,
                     [queue, opt](quint32 code, const QVariantList& rows) {
        std::printf("LIST code=%u rows=%d\n", code, rows.size());
        for (int i = 0; i < rows.size(); ++i) {
            const QVariantMap row = rows.at(i).toMap();
            std::printf("ROW uf=%llu %s size=%llu\n",
                        static_cast<unsigned long long>(
                            row.value(QString::fromUtf8("ufId")).toULongLong()),
                        utf8(row.value(QString::fromUtf8("name")).toString()),
                        static_cast<unsigned long long>(
                            row.value(QString::fromUtf8("size")).toULongLong()));
        }
        std::fflush(stdout);
        static bool enqueued = false;  // 重连会再拉一次列表，任务只入队一次
        if (enqueued) {
            return;
        }
        if (!opt.uploadPath.isEmpty() && queue != nullptr) {
            enqueued = true;
            queue->enqueueUpload(opt.uploadPath, QFileInfo(opt.uploadPath).fileName());
        } else if (!opt.downloadUfId.isEmpty() && queue != nullptr) {
            enqueued = true;
            queue->enqueueDownload(opt.downloadUfId.toULongLong(),
                                   QString::fromUtf8("smoke_download.bin"), 0);
        } else {
            enqueued = true;
            std::printf("SMOKE_OK\n");
            std::fflush(stdout);
            QCoreApplication::exit(0);
        }
    });
    QObject::connect(queue, &cm::transfer::TransferQueue::notice,
                     [](const QString& text, bool error) {
        std::printf("NOTICE %s %s\n", error ? "ERR" : "OK", utf8(text));
        std::fflush(stdout);
    });
    QObject::connect(queue, &cm::transfer::TransferQueue::changed, [queue]() {
        static QHash<int, QString> lastState;
        const QVector<cm::transfer::TransferTask>& tasks = queue->tasks();
        for (int i = 0; i < tasks.size(); ++i) {
            const cm::transfer::TransferTask& task = tasks.at(i);
            if (lastState.value(task.id) == task.stateText()) {
                continue;  // 只在状态变化时打印，避免每块刷屏
            }
            lastState.insert(task.id, task.stateText());
            // 完成时 bitmap 已清空，按 blockCount 显示"全中"，避免 0/5 的误导。
            const quint32 have = task.state == cm::transfer::TaskState::Done
                                     ? task.blockCount
                                     : cm::transfer::bitmapCount(task.bitmap,
                                                                 task.blockCount);
            std::printf("TASK %d %s state=%s bytes=%llu/%llu blocks=%u/%u fetched=%u\n",
                        task.id,
                        task.type == cm::transfer::TaskType::Upload ? "upload" : "download",
                        utf8(task.stateText()),
                        static_cast<unsigned long long>(task.bytesDone),
                        static_cast<unsigned long long>(task.size), have,
                        task.blockCount, task.blocksFetched);
            std::fflush(stdout);
            if (task.isTerminal()) {
                std::printf("SMOKE_OK\n");
                std::fflush(stdout);
                QCoreApplication::exit(0);
            }
        }
    });
    QObject::connect(net, &cm::netapi::NetClient::requestFailed,
                     [](quint16 type, const QString& reason) {
        std::printf("REQ_FAIL type=%u reason=%s\n", type, utf8(reason));
        std::fflush(stdout);
    });
    QTimer::singleShot(60000, []() {
        std::printf("SMOKE_TIMEOUT\n");
        std::fflush(stdout);
        QCoreApplication::exit(3);
    });
    net->login(opt.tel, opt.password);
    return 0;
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
            opt.uploadPath = QString::fromStdString(argv[++i]);
        } else if (arg == "--download" && i + 1 < argc) {
            opt.downloadUfId = QString::fromStdString(argv[++i]);
        } else if (arg == "--ui-demo") {
            opt.demo = true;
        } else if (arg == "--smoke") {
            opt.smoke = true;
        } else if (arg == "--auto-resume") {
            opt.autoResume = true;
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

    if (opt.smoke) {
        if (opt.tel.isEmpty() || opt.password.isEmpty()) {
            std::printf("SMOKE_NEED_CREDENTIALS\n");
            return 1;
        }
        runSmoke(&net, &queue, opt);
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
