#include "ui/ShareDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace cm {
namespace ui {

ShareDialog::ShareDialog(const shareapi::ShareInfo& info, QWidget* parent)
    : QDialog(parent), m_shareCode(info.shareCode) {
    setObjectName(QString::fromUtf8("shareDialog"));
    setWindowTitle(QString::fromUtf8("分享"));
    setMinimumWidth(420);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(14);

    QLabel* title = new QLabel(QString::fromUtf8("分享已创建"), this);
    title->setObjectName(QString::fromUtf8("brandTitle"));
    layout->addWidget(title);

    QLabel* shareCode = new QLabel(info.shareCode, this);
    shareCode->setObjectName(QString::fromUtf8("shareCode"));
    shareCode->setTextInteractionFlags(Qt::TextSelectableByMouse);
    shareCode->setAlignment(Qt::AlignCenter);
    layout->addWidget(shareCode);

    QFormLayout* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->addRow(QString::fromUtf8("提取码"),
                 new QLabel(info.accessCodeRequired && !info.accessCode.isEmpty()
                                ? info.accessCode
                                : QString::fromUtf8("无需提取码"),
                            this));
    form->addRow(QString::fromUtf8("有效期"),
                 new QLabel(shareapi::ShareFlow::expiryText(info.expireAtMs), this));
    layout->addLayout(form);

    QHBoxLayout* copyRow = new QHBoxLayout();
    QPushButton* copyCode = new QPushButton(QString::fromUtf8("复制分享码"), this);
    copyCode->setObjectName(QString::fromUtf8("primaryButton"));
    QPushButton* copyAll = new QPushButton(QString::fromUtf8("复制全部"), this);
    copyRow->addWidget(copyCode);
    copyRow->addWidget(copyAll);
    copyRow->addStretch(1);
    layout->addLayout(copyRow);
    connect(copyCode, &QPushButton::clicked, this, [this]() {
        copy(m_shareCode, QString::fromUtf8("分享码"));
    });
    connect(copyAll, &QPushButton::clicked, this, [this, info]() {
        QString text = m_shareCode;
        if (info.accessCodeRequired && !info.accessCode.isEmpty()) {
            text += QString::fromUtf8("  提取码 %1").arg(info.accessCode);
        }
        copy(text, QString::fromUtf8("分享码与提取码"));
    });

    QHBoxLayout* bottom = new QHBoxLayout();
    QPushButton* cancel = new QPushButton(QString::fromUtf8("取消分享"), this);
    cancel->setObjectName(QString::fromUtf8("dangerButton"));
    QPushButton* close = new QPushButton(QString::fromUtf8("关闭"), this);
    bottom->addWidget(cancel);
    bottom->addStretch(1);
    bottom->addWidget(close);
    layout->addLayout(bottom);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::question(this, QString::fromUtf8("取消分享"),
                                  QString::fromUtf8("取消后该链接立即失效，继续？"))
            != QMessageBox::Yes) {
            return;
        }
        emit cancelRequested(m_shareCode);
        accept();
    });
}

void ShareDialog::copy(const QString& text, const QString& what) {
    QApplication::clipboard()->setText(text);
    QMessageBox::information(this, QString::fromUtf8("已复制"),
                             QString::fromUtf8("%1已复制到剪贴板").arg(what));
}

ShareOpenDialog::ShareOpenDialog(shareapi::ShareFlow* flow, QWidget* parent)
    : QDialog(parent), m_flow(flow), m_codeEdit(nullptr), m_accessEdit(nullptr),
      m_error(nullptr) {
    setObjectName(QString::fromUtf8("shareOpenDialog"));
    setWindowTitle(QString::fromUtf8("打开分享"));
    setMinimumWidth(400);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);

    QLabel* title = new QLabel(QString::fromUtf8("输入分享码"), this);
    title->setObjectName(QString::fromUtf8("brandTitle"));
    layout->addWidget(title);

    m_codeEdit = new QLineEdit(this);
    m_codeEdit->setObjectName(QString::fromUtf8("telEdit"));
    m_codeEdit->setPlaceholderText(QString::fromUtf8("6 位分享码"));
    layout->addWidget(m_codeEdit);

    m_accessEdit = new QLineEdit(this);
    m_accessEdit->setObjectName(QString::fromUtf8("passwordEdit"));
    m_accessEdit->setPlaceholderText(QString::fromUtf8("4 位提取码（没有可留空）"));
    m_accessEdit->setEchoMode(QLineEdit::Normal);
    layout->addWidget(m_accessEdit);

    m_error = new QLabel(this);
    m_error->setObjectName(QString::fromUtf8("formError"));
    m_error->setWordWrap(true);
    layout->addWidget(m_error);

    QHBoxLayout* bottom = new QHBoxLayout();
    QPushButton* cancel = new QPushButton(QString::fromUtf8("取消"), this);
    QPushButton* ok = new QPushButton(QString::fromUtf8("打开"), this);
    ok->setObjectName(QString::fromUtf8("primaryButton"));
    bottom->addWidget(cancel);
    bottom->addStretch(1);
    bottom->addWidget(ok);
    layout->addLayout(bottom);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(ok, &QPushButton::clicked, this, [this]() { accept(); });
    connect(m_codeEdit, &QLineEdit::returnPressed, this, [this]() { accept(); });

    if (m_flow != nullptr) {
        connect(m_flow, &shareapi::ShareFlow::opened, this,
                &ShareOpenDialog::onOpened);
        connect(m_flow, &shareapi::ShareFlow::failed, this,
                &ShareOpenDialog::onFailed);
    }
}

void ShareOpenDialog::accept() {
    m_error->clear();
    if (m_flow == nullptr) {
        QDialog::accept();
        return;
    }
    // 不关窗：等 ShareOpenRs 回来再决定（成功 accept，失败把 2003/2004 写进错误行）。
    m_flow->openShare(m_codeEdit->text(), m_accessEdit->text());
}

void ShareOpenDialog::onOpened() {
    QDialog::accept();
}

void ShareOpenDialog::onFailed(quint32 code, const QString& text) {
    Q_UNUSED(code);
    m_error->setText(text);
}

}  // namespace ui
}  // namespace cm
