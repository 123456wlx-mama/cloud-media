#pragma once

#include <QWidget>

class QListWidget;
class QTabWidget;
class QLabel;

namespace cm {

namespace transfer {
class TransferQueue;
struct TransferTask;
}

namespace ui {

// 传输区：上传/下载两页，任务行 = 名称 + 状态 + 速度 + 进度条 + 操作按钮。
// 只读 TransferQueue 的状态，不自己算进度（契约 §1.3）。
class TransferPane : public QWidget {
    Q_OBJECT

public:
    explicit TransferPane(transfer::TransferQueue* queue, QWidget* parent = nullptr);

public slots:
    void refresh();
    void setLinkText(const QString& text);

private:
    QWidget* buildPage(bool upload);
    QWidget* buildRow(const transfer::TransferTask& task, bool upload);

    transfer::TransferQueue* m_queue;
    QTabWidget* m_tabs;
    QListWidget* m_uploadList;
    QListWidget* m_downloadList;
    QLabel* m_linkHint;
};

}  // namespace ui
}  // namespace cm
