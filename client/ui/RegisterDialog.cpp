#include "ui/RegisterDialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "netapi/NetClient.h"

namespace cm {
namespace ui {

RegisterDialog::RegisterDialog(netapi::NetClient* net, QWidget* parent)
    : QDialog(parent), m_net(net), m_tel(nullptr), m_password(nullptr),
      m_confirm(nullptr), m_name(nullptr), m_error(nullptr), m_ok(nullptr) {
    setWindowTitle(QString::fromUtf8("注册 cloud-media"));
    setObjectName(QString::fromUtf8("registerDialog"));
    setModal(true);
    setFixedSize(380, 500);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(32, 32, 32, 24);
    root->setSpacing(0);

    QLabel* brand = new QLabel(QString::fromUtf8("创建账号"), this);
    brand->setObjectName(QString::fromUtf8("brandTitle"));
    root->addWidget(brand);

    QLabel* subtitle = new QLabel(QString::fromUtf8("注册后返回登录"), this);
    subtitle->setObjectName(QString::fromUtf8("brandSubtitle"));
    root->addWidget(subtitle);
    root->addSpacing(20);

    m_tel = new QLineEdit(this);
    m_tel->setPlaceholderText(QString::fromUtf8("手机号"));
    m_tel->setObjectName(QString::fromUtf8("telEdit"));
    root->addWidget(m_tel);
    root->addSpacing(10);

    m_password = new QLineEdit(this);
    m_password->setPlaceholderText(QString::fromUtf8("密码（至少 6 位）"));
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setObjectName(QString::fromUtf8("passwordEdit"));
    root->addWidget(m_password);
    root->addSpacing(10);

    m_confirm = new QLineEdit(this);
    m_confirm->setPlaceholderText(QString::fromUtf8("确认密码"));
    m_confirm->setEchoMode(QLineEdit::Password);
    m_confirm->setObjectName(QString::fromUtf8("passwordEdit"));
    root->addWidget(m_confirm);
    root->addSpacing(10);

    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(QString::fromUtf8("昵称（可留空）"));
    m_name->setObjectName(QString::fromUtf8("nameEdit"));
    root->addWidget(m_name);
    root->addSpacing(8);

    m_error = new QLabel(this);
    m_error->setObjectName(QString::fromUtf8("formError"));
    m_error->setVisible(false);
    m_error->setWordWrap(true);
    root->addWidget(m_error);
    root->addSpacing(12);

    m_ok = new QPushButton(QString::fromUtf8("注册"), this);
    m_ok->setObjectName(QString::fromUtf8("primaryButton"));
    m_ok->setDefault(true);
    root->addWidget(m_ok);

    QHBoxLayout* footer = new QHBoxLayout();
    footer->setContentsMargins(0, 0, 0, 0);
    QPushButton* back = new QPushButton(QString::fromUtf8("返回登录"), this);
    back->setObjectName(QString::fromUtf8("linkButton"));
    back->setFlat(true);
    footer->addStretch(1);
    footer->addWidget(back);
    root->addLayout(footer);
    root->addStretch(1);

    connect(m_ok, SIGNAL(clicked()), this, SLOT(onRegisterClicked()));
    connect(m_confirm, SIGNAL(returnPressed()), this, SLOT(onRegisterClicked()));
    connect(back, SIGNAL(clicked()), this, SLOT(reject()));
    if (m_net != nullptr) {
        connect(m_net, SIGNAL(registerFinished(quint32, quint64)), this,
                SLOT(onRegisterFinished(quint32, quint64)));
        connect(m_net, SIGNAL(requestFailed(quint16, QString)), this,
                SLOT(onRequestFailed(quint16, QString)));
    }
}

QString RegisterDialog::tel() const { return m_tel->text().trimmed(); }

QString RegisterDialog::password() const { return m_password->text(); }

void RegisterDialog::setBusy(bool busy) {
    m_ok->setEnabled(!busy);
    m_ok->setText(busy ? QString::fromUtf8("注册中…") : QString::fromUtf8("注册"));
}

void RegisterDialog::showError(const QString& text) {
    m_error->setText(text);
    m_error->setVisible(true);
}

void RegisterDialog::onRegisterClicked() {
    if (m_net == nullptr) {
        showError(QString::fromUtf8("网络层未初始化"));
        return;
    }
    if (tel().isEmpty()) {
        showError(QString::fromUtf8("请输入手机号"));
        return;
    }
    if (password().size() < 6) {
        showError(QString::fromUtf8("密码至少 6 位"));
        return;
    }
    if (password() != m_confirm->text()) {
        showError(QString::fromUtf8("两次输入的密码不一致"));
        return;
    }
    QString name = m_name->text().trimmed();
    if (name.isEmpty()) {
        name = QString::fromUtf8("用户") + tel().right(4);
    }
    m_error->setVisible(false);
    setBusy(true);
    m_net->registerAccount(tel(), password(), name);
}

void RegisterDialog::onRegisterFinished(quint32 code, quint64 uid) {
    Q_UNUSED(uid);
    setBusy(false);
    if (code == 0) {
        accept();  // 交给 LoginDialog 回填并提示「注册成功，请登录」
        return;
    }
    if (code == 1003) {
        showError(QString::fromUtf8("该手机号已注册，直接返回登录即可"));
        return;
    }
    showError(QString::fromUtf8("注册失败(code=%1)").arg(code));
}

void RegisterDialog::onRequestFailed(quint16 type, const QString& reason) {
    Q_UNUSED(type);
    setBusy(false);
    showError(QString::fromUtf8("连接失败，请检查网络（%1）").arg(reason));
}

}  // namespace ui
}  // namespace cm
