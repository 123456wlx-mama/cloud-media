#include "ui/RecycleDialog.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "recycleapi/RecycleFlow.h"

namespace cm {
namespace ui {
namespace {

QString formatSize(quint64 bytes) {
    if (bytes < 1024ULL) {
        return QString::fromUtf8("%1 B").arg(bytes);
    }
    if (bytes < 1024ULL * 1024ULL) {
        return QString::fromUtf8("%1 KB").arg(bytes / 1024ULL);
    }
    return QString::fromUtf8("%1 MB").arg(
        QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 1));
}

QString formatDeletedAt(quint64 ms) {
    if (ms == 0) {
        return QString::fromUtf8("—");
    }
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ms))
        .toString(QString::fromUtf8("MM-dd HH:mm"));
}

}  // namespace

RecycleDialog::RecycleDialog(recycleapi::RecycleFlow* flow, QWidget* parent)
    : QDialog(parent), m_flow(flow), m_list(nullptr), m_hint(nullptr) {
    setObjectName(QString::fromUtf8("recycleDialog"));
    setWindowTitle(QString::fromUtf8("回收站"));
    resize(560, 420);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(10);

    m_hint = new QLabel(QString::fromUtf8("已删除的文件会保留在这里，可恢复或彻底删除"), this);
    m_hint->setObjectName(QString::fromUtf8("hintText"));
    layout->addWidget(m_hint);

    m_list = new QListWidget(this);
    m_list->setObjectName(QString::fromUtf8("recycleList"));
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(m_list, 1);

    QHBoxLayout* bottom = new QHBoxLayout();
    QPushButton* close = new QPushButton(QString::fromUtf8("关闭"), this);
    QPushButton* refresh = new QPushButton(QString::fromUtf8("刷新"), this);
    bottom->addWidget(refresh);
    bottom->addStretch(1);
    bottom->addWidget(close);
    layout->addLayout(bottom);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(refresh, &QPushButton::clicked, this, [this]() {
        if (m_flow != nullptr) {
            m_flow->refresh();
        }
    });

    if (m_flow != nullptr) {
        connect(m_flow, &recycleapi::RecycleFlow::listChanged, this,
                &RecycleDialog::rebuild);
        connect(m_flow, &recycleapi::RecycleFlow::restored, this,
                [this](quint64, const QString&) {
                    if (m_flow != nullptr) {
                        m_flow->refresh();
                    }
                });
        connect(m_flow, &recycleapi::RecycleFlow::purged, this,
                [this](quint64) {
                    if (m_flow != nullptr) {
                        m_flow->refresh();
                    }
                });
        connect(m_flow, &recycleapi::RecycleFlow::failed, this,
                [this](const QString& text) {
                    QMessageBox::warning(this, QString::fromUtf8("回收站"), text);
                });
    }
    rebuild();
}

void RecycleDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (m_flow != nullptr) {
        m_flow->refresh();
    }
}

void RecycleDialog::rebuild() {
    if (m_flow == nullptr || m_list == nullptr) {
        return;
    }
    m_list->clear();
    const QVector<recycleapi::RecycleEntry>& items = m_flow->entries();
    m_hint->setText(QString::fromUtf8("回收站 · %1 项").arg(items.size()));
    for (int i = 0; i < items.size(); ++i) {
        const recycleapi::RecycleEntry& entry = items.at(i);
        QWidget* row = new QWidget(m_list);
        row->setObjectName(QString::fromUtf8("recycleRow"));
        QHBoxLayout* layout = new QHBoxLayout(row);
        layout->setContentsMargins(10, 6, 10, 6);
        layout->setSpacing(10);

        QLabel* name = new QLabel(
            entry.isDir ? QString::fromUtf8("📁 %1").arg(entry.name) : entry.name, row);
        name->setObjectName(QString::fromUtf8("taskName"));
        layout->addWidget(name, 1);

        QLabel* meta = new QLabel(
            QString::fromUtf8("%1 · 删除于 %2")
                .arg(entry.isDir ? QString::fromUtf8("文件夹") : formatSize(entry.size),
                     formatDeletedAt(entry.deletedAtMs)),
            row);
        meta->setObjectName(QString::fromUtf8("hintText"));
        layout->addWidget(meta);

        const quint64 ufId = entry.ufId;
        // C++11 没有 init-capture，lambda 里要用到的字段先取成局部变量
        const bool isDir = entry.isDir;
        const QString entryName = entry.name;
        QPushButton* restore = new QPushButton(QString::fromUtf8("恢复"), row);
        restore->setObjectName(QString::fromUtf8("rowButton"));
        connect(restore, &QPushButton::clicked, this, [this, ufId]() {
            if (m_flow != nullptr) {
                m_flow->restore(ufId);
            }
        });
        layout->addWidget(restore);

        QPushButton* purge = new QPushButton(QString::fromUtf8("彻底删除"), row);
        purge->setObjectName(QString::fromUtf8("dangerButton"));
        connect(purge, &QPushButton::clicked, this, [this, ufId, isDir, entryName]() {
            if (m_flow == nullptr) {
                return;
            }
            // 文件夹的彻底删除会级联删掉整棵子树（服务端 purge 递归收集子节点），
            // 提示必须写清楚，否则用户以为只删了一个空目录。
            const QString text =
                isDir ? QString::fromUtf8("「%1」及其内部所有内容都会被彻底删除，"
                                          "无法恢复。继续？")
                            .arg(entryName)
                      : QString::fromUtf8("彻底删除后无法恢复，继续？");
            if (QMessageBox::question(this, QString::fromUtf8("彻底删除"), text) ==
                QMessageBox::Yes) {
                m_flow->purge(ufId);
            }
        });
        layout->addWidget(purge);

        QListWidgetItem* item = new QListWidgetItem(m_list);
        item->setSizeHint(QSize(0, 44));
        m_list->setItemWidget(item, row);
    }
    if (items.isEmpty()) {
        m_hint->setText(QString::fromUtf8("回收站是空的"));
    }
}


}  // namespace ui
}  // namespace cm
