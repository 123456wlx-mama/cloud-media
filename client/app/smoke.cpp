#include "app/smoke.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <cstdarg>
#include <cstdio>

#include "netapi/NetClient.h"
#include "recycleapi/RecycleFlow.h"
#include "shareapi/ShareFlow.h"
#include "transfer/TransferQueue.h"

namespace cm {
namespace app {
namespace {

const quint32 kDefaultLimit = 50;
const int kSmokeTimeoutMs = 90000;

// 每次调用返回独立 QByteArray：同一行 printf 里多个 %s 不会互相覆盖。
QByteArray u8(const QString& text) { return text.toUtf8(); }

void say(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::fflush(stdout);
}

// runSmoke 返回后事件循环仍在跑：状态必须活过栈帧。
struct SmokeState {
    SmokeOptions opt;
    bool paged;
    bool acted;
    bool cdDone;
    bool deleted;
    bool undoStarted;
    bool verifyList;
    bool purgeStarted;
    QString deleteName;

    quint32 limit() const {
        return opt.pageLimit == 0 ? kDefaultLimit : opt.pageLimit;
    }
};

// 文件作用域：runSmoke 返回后事件循环继续跑，状态必须活过栈帧。
SmokeState g_state;

}  // namespace

int runSmoke(netapi::NetClient* net, transfer::TransferQueue* queue,
             shareapi::ShareFlow* share, recycleapi::RecycleFlow* recycle,
             const SmokeOptions& opt) {
    g_state = SmokeState();
    g_state.opt = opt;

    QObject::connect(net, &netapi::NetClient::linkState,
                     [](int state, const QString& detail) {
        say("LINK state=%d detail=%s\n", state, u8(detail).constData());
    });

    QObject::connect(net, &netapi::NetClient::loginFinished,
                     [net, queue](quint32 code, quint64 uid, const QString& name) {
        if (code != 0) {
            say("LOGIN_FAIL code=%u\n", code);
            QCoreApplication::exit(2);
            return;
        }
        say("LOGIN_OK uid=%llu name=%s\n",
            static_cast<unsigned long long>(uid), u8(name).constData());
        if (g_state.opt.autoResume && queue != nullptr) {
            const QVector<transfer::TransferTask> tasks = queue->tasks();
            for (int i = 0; i < tasks.size(); ++i) {
                const transfer::TransferTask task = tasks.at(i);
                if (task.state != transfer::TaskState::Paused) {
                    continue;
                }
                say("RESUME task=%d bytesDone=%llu blocks=%u/%u\n", task.id,
                    static_cast<unsigned long long>(task.bytesDone),
                    transfer::bitmapCount(task.bitmap, task.blockCount),
                    task.blockCount);
                queue->resume(task.id);
            }
        }
        net->listFiles(0, g_state.limit());
    });

    QObject::connect(net, &netapi::NetClient::listFinished,
                     [net, queue, share, recycle](
                         quint32 code, const QVariantList& rows, bool hasMore,
                         const QString& nextKey, quint64 nextId) {
        say("LIST code=%u rows=%d hasMore=%d\n", code, rows.size(),
            hasMore ? 1 : 0);
        for (int i = 0; i < rows.size(); ++i) {
            const QVariantMap row = rows.at(i).toMap();
            const quint64 ufId =
                row.value(QString::fromUtf8("ufId")).toULongLong();
            const QString rowName =
                row.value(QString::fromUtf8("name")).toString();
            say("ROW uf=%llu %s size=%llu dir=%d\n",
                static_cast<unsigned long long>(ufId), u8(rowName).constData(),
                static_cast<unsigned long long>(
                    row.value(QString::fromUtf8("size")).toULongLong()),
                row.value(QString::fromUtf8("isDir")).toBool() ? 1 : 0);
            if (g_state.opt.deleteUfId.toULongLong() == ufId) {
                g_state.deleteName = rowName;
            }
        }
        if (code != 0) {
            say("SMOKE_FAIL list code=%u\n", code);
            QCoreApplication::exit(4);
            return;
        }
        if (g_state.verifyList) {
            say("SMOKE_OK\n");
            QCoreApplication::exit(0);
            return;
        }
        if (!g_state.opt.cdUfId.isEmpty() && !g_state.cdDone) {
            g_state.cdDone = true;
            say("CD uf=%s\n", u8(g_state.opt.cdUfId).constData());
            net->listFiles(g_state.opt.cdUfId.toULongLong(), g_state.limit());
            return;
        }
        if (hasMore && !g_state.paged) {
            g_state.paged = true;
            say("PAGE next limit=%u\n", g_state.limit());
            net->listFiles(0, g_state.limit(), nextKey, nextId);
            return;
        }
        if (g_state.acted) {
            return;
        }
        g_state.acted = true;

        if (!g_state.opt.shareCreateUfId.isEmpty() && share != nullptr) {
            say("SHARE_CREATE uf=%s\n", u8(g_state.opt.shareCreateUfId).constData());
            share->create(g_state.opt.shareCreateUfId.toULongLong(), 2, 1);
            return;
        }
        if (!g_state.opt.shareCancelCode.isEmpty() && share != nullptr) {
            say("SHARE_CANCEL code=%s\n",
                u8(g_state.opt.shareCancelCode).constData());
            share->cancel(g_state.opt.shareCancelCode);
            return;
        }
        if (!g_state.opt.shareOpenCode.isEmpty() && share != nullptr) {
            say("SHARE_OPEN code=%s access=%s\n",
                u8(g_state.opt.shareOpenCode).constData(),
                u8(g_state.opt.accessCode).constData());
            share->openShare(g_state.opt.shareOpenCode, g_state.opt.accessCode);
            return;
        }
        if (!g_state.opt.deleteUfId.isEmpty() && recycle != nullptr) {
            const quint64 ufId = g_state.opt.deleteUfId.toULongLong();
            if (g_state.deleteName.isEmpty()) {
                g_state.deleteName = QString::fromUtf8("uf_%1").arg(ufId);
            }
            say("DELETE uf=%llu name=%s\n",
                static_cast<unsigned long long>(ufId),
                u8(g_state.deleteName).constData());
            recycle->deleteFile(ufId, g_state.deleteName);
            return;
        }
        if (!g_state.opt.uploadPath.isEmpty() && queue != nullptr) {
            queue->enqueueUpload(g_state.opt.uploadPath,
                                 QFileInfo(g_state.opt.uploadPath).fileName());
            return;
        }
        if (!g_state.opt.downloadUfId.isEmpty() && queue != nullptr) {
            queue->enqueueDownload(g_state.opt.downloadUfId.toULongLong(),
                                   QString::fromUtf8("smoke_download.bin"), 0);
            return;
        }
        say("SMOKE_OK\n");
        QCoreApplication::exit(0);
    });

    if (share != nullptr) {
        QObject::connect(share, &shareapi::ShareFlow::created, [share]() {
            const shareapi::ShareInfo& info = share->info();
            say("SHARE code=%s access=%s required=%d expire=%s\n",
                u8(info.shareCode).constData(), u8(info.accessCode).constData(),
                info.accessCodeRequired ? 1 : 0,
                u8(shareapi::ShareFlow::expiryText(info.expireAtMs)).constData());
            QCoreApplication::exit(0);
        });
        QObject::connect(share, &shareapi::ShareFlow::opened, [share, queue]() {
            const shareapi::ShareInfo& info = share->info();
            say("SHARE_OPEN_OK file=%s size=%llu ticket=%s\n",
                u8(info.fileName).constData(),
                static_cast<unsigned long long>(info.size),
                info.shareTicket.isEmpty() ? "none" : "yes");
            if (queue != nullptr && !info.shareTicket.isEmpty()) {
                queue->enqueueSharedDownload(info.shareTicket, info.fileName,
                                             info.size);
            }
        });
        QObject::connect(share, &shareapi::ShareFlow::cancelled,
                         [](const QString& code) {
            say("SHARE_CANCELLED %s\n", u8(code).constData());
            QCoreApplication::exit(0);
        });
        QObject::connect(share, &shareapi::ShareFlow::failed,
                         [](quint32 code, const QString& text) {
            say("SHARE_FAIL code=%u text=%s\n", code, u8(text).constData());
            QCoreApplication::exit(4);
        });
    }

    if (recycle != nullptr) {
        QObject::connect(recycle, &recycleapi::RecycleFlow::deleted,
                         [recycle](quint64 ufId, const QString& name) {
            say("DELETE_OK uf=%llu name=%s\n",
                static_cast<unsigned long long>(ufId), u8(name).constData());
            g_state.deleted = true;
            recycle->refresh();
        });
        QObject::connect(recycle, &recycleapi::RecycleFlow::listChanged,
                         [recycle]() {
            const QVector<recycleapi::RecycleEntry>& items = recycle->entries();
            say("RECYCLE_COUNT %d\n", items.size());
            for (int i = 0; i < items.size(); ++i) {
                say("RECYCLE_ITEM uf=%llu %s size=%llu\n",
                    static_cast<unsigned long long>(items.at(i).ufId),
                    u8(items.at(i).name).constData(),
                    static_cast<unsigned long long>(items.at(i).size));
            }
            if (g_state.deleted && !g_state.undoStarted && g_state.opt.purgeUfId.isEmpty()) {
                g_state.undoStarted = true;
                say("UNDO\n");
                recycle->undo();
            } else if (g_state.deleted && !g_state.purgeStarted &&
                       !g_state.opt.purgeUfId.isEmpty()) {
                g_state.purgeStarted = true;
                say("PURGE uf=%s\n", u8(g_state.opt.purgeUfId).constData());
                recycle->purge(g_state.opt.purgeUfId.toULongLong());
            }
        });
        QObject::connect(recycle, &recycleapi::RecycleFlow::restored,
                         [net, recycle](quint64 ufId, const QString& name) {
            say("RESTORE_OK uf=%llu name=%s\n",
                static_cast<unsigned long long>(ufId), u8(name).constData());
            g_state.verifyList = true;
            recycle->refresh();                 // 回收站里应不再出现
            net->listFiles(0, g_state.limit());      // 文件区应重新出现
        });
        QObject::connect(recycle, &recycleapi::RecycleFlow::purged,
                         [](quint64 ufId) {
            say("PURGE_OK uf=%llu\n", static_cast<unsigned long long>(ufId));
            QCoreApplication::exit(0);
        });
        QObject::connect(recycle, &recycleapi::RecycleFlow::failed,
                         [](const QString& text) {
            say("RECYCLE_FAIL %s\n", u8(text).constData());
            QCoreApplication::exit(4);
        });
    }

    if (queue != nullptr) {
        QObject::connect(queue, &transfer::TransferQueue::notice,
                         [](const QString& text, bool error) {
            say("NOTICE %s %s\n", error ? "ERR" : "OK", u8(text).constData());
        });
        QObject::connect(queue, &transfer::TransferQueue::changed, [queue]() {
            static QHash<int, QString> lastState;
            const QVector<transfer::TransferTask>& tasks = queue->tasks();
            for (int i = 0; i < tasks.size(); ++i) {
                const transfer::TransferTask& task = tasks.at(i);
                if (lastState.value(task.id) == task.stateText()) {
                    continue;
                }
                lastState.insert(task.id, task.stateText());
                const quint32 have =
                    task.state == transfer::TaskState::Done
                        ? task.blockCount
                        : transfer::bitmapCount(task.bitmap, task.blockCount);
                say("TASK %d %s state=%s bytes=%llu/%llu blocks=%u/%u fetched=%u\n",
                    task.id,
                    task.type == transfer::TaskType::Upload ? "upload" : "download",
                    u8(task.stateText()).constData(),
                    static_cast<unsigned long long>(task.bytesDone),
                    static_cast<unsigned long long>(task.size), have,
                    task.blockCount, task.blocksFetched);
                if (task.isTerminal()) {
                    say("SMOKE_OK\n");
                    QCoreApplication::exit(0);
                }
            }
        });
        QObject::connect(queue, &transfer::TransferQueue::instantUploaded,
                         [](const QString& name) {
            say("INSTANT %s\n", u8(name).constData());
        });
    }

    QObject::connect(net, &netapi::NetClient::requestAborted,
                     [](quint32 seq, quint16 type, const QString& reason) {
        say("REQ_ABORT seq=%u type=%u reason=%s\n", seq, type,
            u8(reason).constData());
    });
    QObject::connect(net, &netapi::NetClient::requestFailed,
                     [](quint16 type, const QString& reason) {
        say("REQ_FAIL type=%u reason=%s\n", type, u8(reason).constData());
    });

    QTimer::singleShot(kSmokeTimeoutMs, []() {
        say("SMOKE_TIMEOUT\n");
        QCoreApplication::exit(3);
    });
    return 0;
}

}  // namespace app
}  // namespace cm
