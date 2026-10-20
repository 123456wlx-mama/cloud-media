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
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "app/smoke.h"
#include "player/PlayerCore.h"
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

// 里程碑 A 取证：本地完整文件播放 + 暂停 + seek，无界面输出。
int runPlayFile(const QString& path) {
    static cm::player::PlayerCore player;  // 必须在事件循环内存活
    static QElapsedTimer clock;
    static int frames = 0;
    clock.start();

    QObject::connect(&player, &cm::player::PlayerCore::opened,
                     [](qint64 durationMs, int w, int h) {
        std::printf("PLAY_OPEN duration_ms=%lld %dx%d\n",
                    static_cast<long long>(durationMs), w, h);
        std::fflush(stdout);
    });
    QObject::connect(&player, &cm::player::PlayerCore::frameReady,
                     [](const QImage&) {
        if (frames == 0) {
            std::printf("PLAY_FIRST_FRAME at_ms=%lld\n",
                        static_cast<long long>(clock.elapsed()));
            std::fflush(stdout);
        }
        ++frames;
    });
    QObject::connect(&player, &cm::player::PlayerCore::positionChanged,
                     [](qint64 ms) {
        static qint64 lastSecond = -1;
        const qint64 second = ms / 1000;
        if (second != lastSecond && (second % 2) == 0) {
            lastSecond = second;
            std::printf("PLAY_POS ms=%lld\n", static_cast<long long>(ms));
            std::fflush(stdout);
        }
    });
    QObject::connect(&player, &cm::player::PlayerCore::ended, []() {
        std::printf("PLAY_ENDED frames=%d\n", frames);
        std::fflush(stdout);
        QCoreApplication::exit(0);
    });
    QObject::connect(&player, &cm::player::PlayerCore::errorChanged,
                     [](const QString& text) {
        std::printf("PLAY_ERROR %s\n", text.toUtf8().constData());
        std::fflush(stdout);
        QCoreApplication::exit(5);
    });

    player.open(path, false);
    player.play();
    QTimer::singleShot(1500, []() {
        player.pause();
        std::printf("PLAY_PAUSE pos_ms=%lld\n",
                    static_cast<long long>(player.position()));
        std::fflush(stdout);
    });
    QTimer::singleShot(2000, []() {
        player.play();
        std::printf("PLAY_RESUME\n");
        std::fflush(stdout);
    });
    QTimer::singleShot(2500, []() {
        player.seek(30000);
        std::printf("PLAY_SEEK 30000\n");
        std::fflush(stdout);
    });
    QTimer::singleShot(3500, []() {
        std::printf("PLAY_AFTER_SEEK pos_ms=%lld frames=%d\n",
                    static_cast<long long>(player.position()), frames);
        std::fflush(stdout);
    });
    QTimer::singleShot(4500, []() {
        player.close();
        std::printf("PLAY_SMOKE_OK frames=%d\n", frames);
        std::fflush(stdout);
        QCoreApplication::exit(0);
    });
    return 0;
}

// 里程碑 B 取证：边下边播。双击文件 → playback 下载写 .part → 播放器读这个增长中的文件。
int runPlaybackSmoke(cm::netapi::NetClient* net, cm::transfer::TransferQueue* queue,
                     const QString& ufId, const QString& shareCode,
                     const QString& accessCode) {
    static cm::player::PlayerCore player;
    static QElapsedTimer clock;
    static int frames = 0;
    static int taskId = 0;
    static bool sourceDone = false;
    static cm::shareapi::ShareFlow* share = nullptr;
    clock.start();

    QObject::connect(net, &cm::netapi::NetClient::loginFinished,
                     [net, shareCode, accessCode](quint32 code, quint64,
                                                  const QString&) {
        if (code != 0) {
            std::printf("LOGIN_FAIL code=%u\n", code);
            std::fflush(stdout);
            QCoreApplication::exit(2);
            return;
        }
        if (!shareCode.isEmpty() && share != nullptr) {
            std::printf("SHARE_OPEN code=%s access=%s\n",
                        shareCode.toUtf8().constData(),
                        accessCode.toUtf8().constData());
            std::fflush(stdout);
            share->openShare(shareCode, accessCode);
            return;
        }
        net->listFiles(0, 50);  // 先拿列表，才能把 ufId 映射成文件名/大小
    });

    if (!shareCode.isEmpty()) {
        share = new cm::shareapi::ShareFlow(net);
        QObject::connect(share, &cm::shareapi::ShareFlow::opened, [queue]() {
            const cm::shareapi::ShareInfo& info = share->info();
            std::printf("SHARE_OPEN_OK code=%s file=%s size=%llu ticket=yes\n",
                        info.shareCode.toUtf8().constData(),
                        info.fileName.toUtf8().constData(),
                        static_cast<unsigned long long>(info.size));
            taskId = queue->enqueueSharedPlayback(info.shareTicket, info.fileName,
                                                  info.size);
            std::printf("PLAY_STREAM_QUEUE task=%d file=%s size=%llu\n", taskId,
                        info.fileName.toUtf8().constData(),
                        static_cast<unsigned long long>(info.size));
            std::fflush(stdout);
        });
        QObject::connect(share, &cm::shareapi::ShareFlow::failed,
                         [](quint32 code, const QString& text) {
            std::printf("SHARE_FAIL code=%u text=%s\n", code,
                        text.toUtf8().constData());
            std::fflush(stdout);
            QCoreApplication::exit(4);
        });
    }
    QObject::connect(net, &cm::netapi::NetClient::listFinished,
                     [queue, ufId](quint32 code, const QVariantList& rows, bool,
                                   const QString&, quint64) {
        if (code != 0) {
            std::printf("LIST_FAIL code=%u\n", code);
            std::fflush(stdout);
            QCoreApplication::exit(2);
            return;
        }
        if (ufId.isEmpty()) {
            return;
        }
        const QString target = ufId;
        QString name;
        quint64 size = 0;
        for (int i = 0; i < rows.size(); ++i) {
            const QVariantMap row = rows.at(i).toMap();
            if (QString::number(row.value(QString::fromUtf8("ufId")).toULongLong()) ==
                target) {
                name = row.value(QString::fromUtf8("name")).toString();
                size = row.value(QString::fromUtf8("size")).toULongLong();
                break;
            }
        }
        if (name.isEmpty()) {
            std::printf("PLAY_STREAM_FAIL file_not_found uf=%s\n",
                        target.toUtf8().constData());
            std::fflush(stdout);
            QCoreApplication::exit(2);
            return;
        }
        taskId = queue->enqueuePlaybackDownload(target.toULongLong(), name, size);
        const cm::transfer::TransferTask* task = queue->find(taskId);
        const QString part = task->localPath + QString::fromUtf8(".part");
        std::printf("PLAY_STREAM_QUEUE task=%d file=%s size=%llu part=%s\n", taskId,
                    name.toUtf8().constData(),
                    static_cast<unsigned long long>(size),
                    part.toUtf8().constData());
        // 不立刻 open：要等首块落盘，否则 avformat_open_input 会在 GUI 线程里等数据，
        // 而下载响应也走 GUI 线程的事件循环 → 死等。
    });
    QObject::connect(queue, &cm::transfer::TransferQueue::changed, [queue]() {
        static bool playerOpened = false;
        if (taskId == 0) {
            return;
        }
        const cm::transfer::TransferTask* task = queue->find(taskId);
        if (task == nullptr) {
            return;
        }
        // 首块写盘后再 open：首块 4MB 足够 moov + 探测
        if (!playerOpened && task->bytesDone > 0 && task->state == cm::transfer::TaskState::Running) {
            playerOpened = true;
            const QString part = task->localPath + QString::fromUtf8(".part");
            std::printf("PLAY_STREAM_OPEN_REQUEST part=%s at_ms=%lld\n",
                        part.toUtf8().constData(),
                        static_cast<long long>(clock.elapsed()));
            std::fflush(stdout);
            player.open(part, true);
            player.play();
        }
        if ((task->state == cm::transfer::TaskState::Done ||
             task->state == cm::transfer::TaskState::Failed ||
             task->state == cm::transfer::TaskState::Canceled) &&
            !sourceDone) {
            sourceDone = true;
            static bool jumpedToEnd = false;
            player.setSourceComplete(true);
            std::printf("PLAY_STREAM_SOURCE_DONE state=%s bytes=%llu\n",
                        task->stateText().toUtf8().constData(),
                        static_cast<unsigned long long>(task->bytesDone));
            std::fflush(stdout);
            // 下载完成后跳到结尾附近，验证"播到真正的文件末尾会自然结束"
            // 短片段自然播完（demo 用 20s 片），长视频才跳到结尾取证
            if (!jumpedToEnd && task->state == cm::transfer::TaskState::Done &&
                player.duration() > 60000) {
                jumpedToEnd = true;
                const qint64 target = player.duration() - 3000;
                std::printf("PLAY_STREAM_SEEK_NEAR_END ms=%lld\n",
                            static_cast<long long>(target));
                std::fflush(stdout);
                player.seek(target);
            }
        }
    });
    QObject::connect(&player, &cm::player::PlayerCore::opened,
                     [](qint64 durationMs, int w, int h) {
        std::printf("PLAY_STREAM_OPEN duration_ms=%lld %dx%d at_ms=%lld\n",
                    static_cast<long long>(durationMs), w, h,
                    static_cast<long long>(clock.elapsed()));
        std::fflush(stdout);
    });
    QObject::connect(&player, &cm::player::PlayerCore::frameReady,
                     [](const QImage&) {
        if (frames == 0) {
            std::printf("PLAY_STREAM_FIRST_FRAME at_ms=%lld\n",
                        static_cast<long long>(clock.elapsed()));
            std::fflush(stdout);
        }
        ++frames;
    });
    QObject::connect(&player, &cm::player::PlayerCore::bufferingChanged,
                     [](bool buffering) {
        std::printf("PLAY_STREAM_BUFFERING %d at_ms=%lld\n", buffering ? 1 : 0,
                    static_cast<long long>(clock.elapsed()));
        std::fflush(stdout);
    });
    QObject::connect(&player, &cm::player::PlayerCore::positionChanged,
                     [](qint64 ms) {
        static qint64 lastBucket = -1;
        const qint64 bucket = ms / 5000;
        if (bucket != lastBucket) {
            lastBucket = bucket;
            std::printf("PLAY_STREAM_POS ms=%lld frames=%d\n",
                        static_cast<long long>(ms), frames);
            std::fflush(stdout);
        }
    });
    QObject::connect(&player, &cm::player::PlayerCore::ended, []() {
        std::printf("PLAY_STREAM_ENDED frames=%d\n", frames);
        std::fflush(stdout);
        QCoreApplication::exit(0);
    });
    QObject::connect(&player, &cm::player::PlayerCore::errorChanged,
                     [](const QString& text) {
        std::printf("PLAY_STREAM_ERROR %s\n", text.toUtf8().constData());
        std::fflush(stdout);
        QCoreApplication::exit(5);
    });
    QTimer::singleShot(180000, []() {
        std::printf("PLAY_STREAM_TIMEOUT\n");
        std::fflush(stdout);
        QCoreApplication::exit(3);
    });
    return 0;
}

struct Options {
    QString host;
    quint16 port;
    QString tel;
    QString password;
    cm::app::SmokeOptions smoke;
    bool demo;
    bool smokeMode;
    QString screenshot;
    QString playFile;
    QString playUfId;
    QString playLocal;
    QString playRemoteUfId;
    QString playShareCode;
    QString shareUfId;
    int shotDelayMs;

    Options() : host(QString::fromUtf8("127.0.0.1")), port(8004),
                demo(false), smokeMode(false), shotDelayMs(2500) {}
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
        } else if (arg == "--play-file" && i + 1 < argc) {
            opt.playFile = QString::fromStdString(argv[++i]);
        } else if (arg == "--play-local" && i + 1 < argc) {
            opt.playLocal = QString::fromStdString(argv[++i]);
        } else if (arg == "--ui-share" && i + 1 < argc) {
            opt.shareUfId = QString::fromStdString(argv[++i]);
        } else if (arg == "--play-share" && i + 1 < argc) {
            opt.playShareCode = QString::fromStdString(argv[++i]);
        } else if (arg == "--play-remote" && i + 1 < argc) {
            opt.playRemoteUfId = QString::fromStdString(argv[++i]);
        } else if (arg == "--shot-delay" && i + 1 < argc) {
            opt.shotDelayMs = std::atoi(argv[++i]);
        } else if (arg == "--play-uf" && i + 1 < argc) {
            opt.playUfId = QString::fromStdString(argv[++i]);
        } else if (arg == "--screenshot" && i + 1 < argc) {
            opt.screenshot = QString::fromStdString(argv[++i]);
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName(QString::fromUtf8("cloud-media"));
    applyTheme(&app);

    if (!opt.playFile.isEmpty()) {
        runPlayFile(opt.playFile);
        return app.exec();
    }

    cm::sqlapi::TaskStore store;
    store.open(QString::fromUtf8("cm-tasks.db"));

    cm::netapi::NetClient net;
    cm::transfer::TransferQueue queue(&net, &store);
    queue.restoreFromStore();
    net.connectTo(opt.host, opt.port);

    if (!opt.playUfId.isEmpty() || !opt.playShareCode.isEmpty()) {
        runPlaybackSmoke(&net, &queue, opt.playUfId, opt.playShareCode,
                         opt.smoke.accessCode);
        net.login(opt.tel, opt.password);
        return app.exec();
    }

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
    if (!opt.playLocal.isEmpty()) {
        window.playLocalFile(opt.playLocal);
    }
    if (!opt.playRemoteUfId.isEmpty()) {
        const quint64 ufId = opt.playRemoteUfId.toULongLong();
        // 等列表回来再点播（列表是登录后异步拉的）
        QTimer::singleShot(1500, [&window, ufId]() { window.playRemoteByUfId(ufId); });
    }
    if (!opt.shareUfId.isEmpty()) {
        const quint64 ufId = opt.shareUfId.toULongLong();
        QTimer::singleShot(1500, [&window, ufId]() { window.shareByUfId(ufId); });
    }
    if (!opt.screenshot.isEmpty()) {
        const QString path = opt.screenshot;
        QTimer::singleShot(opt.shotDelayMs, [&window, path]() {
            // 分享窗是模态框，截它而不是主窗口
            QWidget* target = QApplication::activeModalWidget();
            if (target == nullptr) {
                target = &window;
            }
            target->grab().save(path);
            QApplication::quit();
        });
    }
    return app.exec();
}
