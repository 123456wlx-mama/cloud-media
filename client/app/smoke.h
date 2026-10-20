#pragma once

#include <QString>
#include <QtGlobal>

namespace cm {

namespace netapi {
class NetClient;
}

namespace transfer {
class TransferQueue;
}

namespace shareapi {
class ShareFlow;
}

namespace recycleapi {
class RecycleFlow;
}

namespace app {

// 无界面联调参数（--smoke 专用）。空字符串/0 表示该步不做。
struct SmokeOptions {
    QString uploadPath;
    QString downloadUfId;
    QString shareCreateUfId;   // --share-create <ufId>
    QString shareOpenCode;     // --share-open <code>
    QString shareCancelCode;   // --share-cancel <code>
    QString accessCode;        // --access <4 位>
    QString deleteUfId;        // --delete <ufId>
    QString purgeUfId;         // --purge <ufId>（与 --delete 连用）
    QString cdUfId;            // --cd <ufId>：进目录再列一次
    bool autoResume;
    quint32 pageLimit;         // 0 = 用默认 50

    SmokeOptions() : autoResume(false), pageLimit(0) {}
};

// 登录 → 列表（含翻页）→ 可选：秒传/分享/打开分享/删除+回收站/进目录。
// 输出为一行一条的机器可读文本，便于做 P8 联调证据。
int runSmoke(netapi::NetClient* net, transfer::TransferQueue* queue,
             shareapi::ShareFlow* share, recycleapi::RecycleFlow* recycle,
             const SmokeOptions& opt);

}  // namespace app
}  // namespace cm
