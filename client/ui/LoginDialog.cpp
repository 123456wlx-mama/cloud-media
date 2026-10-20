#include "ui/LoginDialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "netapi/NetClient.h"

namespace cm {
namespace ui {

LoginDialog::LoginDialog(netapi::NetClient* net, QWidget* parent)
    : QDialog(parent), m_net(net), m_tel(nullptr), m_password(nullptr),
      m_error(nullptr), m_loginButton(nullptr) {
    setWindowTitle(QString::fromUtf8("登录 cloud-media"));
    setObjectName(QString::fromUtf8("loginDialog"));
    setModal(true);
    setFixedSize(380, 420);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(32, 36, 32, 28);
    root->setSpacing(0);

    QLabel* brand = new QLabel(QString::fromUtf8("cloud-media"), this);
    brand->setObjectName(QString::fromUtf8("brandTitle"));
    root->addWidget(brand);

    QLabel* subtitle =
        new QLabel(QString::fromUtf8("登录后同步你的文件与传输任务"), this);
    subtitle->setObjectName(QString::fromUtf8("brandSubtitle"));
    root->addWidget(subtitle);
    root->addSpacing(28);

    m_tel = new QLineEdit(this);
    m_tel->setPlaceholderText(QString::fromUtf8("手机号"));
    m_tel->setObjectName(QString::fromUtf8("telEdit"));
    root->addWidget(m_tel);
    root->addSpacing(12);

    m_password = new QLineEdit(this);
    m_password->setPlaceholderText(QString::fromUtf8("密码"));
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setObjectName(QString::fromUtf8("passwordEdit"));
    root->addWidget(m_password);
    root->addSpacing(8);

    m_error = new QLabel(this);
    m_error->setObjectName(QString::fromUtf8("formError"));
    m_error->setVisible(false);
    m_error->setWordWrap(true);
    root->addWidget(m_error);
    root->addSpacing(16);

    m_loginButton = new QPushButton(QString::fromUtf8("登录"), this);
    m_loginButton->setObjectName(QString::fromUtf8("primaryButton"));
    m_loginButton->setDefault(true);
    root->addWidget(m_loginButton);
    root->addStretch(1);

    QHBoxLayout* footer = new QHBoxLayout();
    footer->setContentsMargins(0, 0, 0, 0);
    QLabel* tip = new QLabel(QString::fromUtf8("还没有账号？"), this);
    tip->setObjectName(QString::fromUtf8("hintText"));
    QPushButton* reg = new QPushButton(QString::fromUtf8("注册"), this);
    reg->setObjectName(QString::fromUtf8("linkButton"));
    reg->setFlat(true);
    footer->addWidget(tip);
    footer->addWidget(reg);
    footer->addStretch(1);
    root->addLayout(footer);

    connect(m_loginButton, SIGNAL(clicked()), this, SLOT(onLoginClicked()));
    connect(m_tel, SIGNAL(returnPressed()), this, SLOT(onLoginClicked()));
    connect(m_password, SIGNAL(returnPressed()), this, SLOT(onLoginClicked()));
    if (m_net != nullptr) {
        connect(m_net, SIGNAL(loginFinished(quint32, quint64, QString)), this,
                SLOT(onLoginFinished(quint32, quint64, QString)));
        connect(m_net, SIGNAL(requestFailed(quint16, QString)), this,
                SLOT(onRequestFailed(quint16, QString)));
    }
}

QString LoginDialog::tel() const { return m_tel->text().trimmed(); }

QString LoginDialog::password() const { return m_password->text(); }

void LoginDialog::setBusy(bool busy) {
    m_loginButton->setEnabled(!busy);
    m_loginButton->setText(busy ? QString::fromUtf8("登录中…")
                                : QString::fromUtf8("登录"));
}

void LoginDialog::showError(const QString& text) {
    m_error->setText(text);
    m_error->setVisible(true);
}

void LoginDialog::onLoginClicked() {
    if (m_net == nullptr) {
        showError(QString::fromUtf8("网络层未初始化"));
        return;
    }
    if (tel().isEmpty() || password().isEmpty()) {
        showError(QString::fromUtf8("请输入手机号与密码"));
        return;
    }
    m_error->setVisible(false);
    setBusy(true);
    m_net->login(tel(), password());
}

void LoginDialog::onLoginFinished(quint32 code, quint64 uid, const QString& name) {
    Q_UNUSED(uid);
    Q_UNUSED(name);
    if (code == 0) {
        accept();
        return;
    }
    setBusy(false);
    // 契约 §2.1：2001/2004 → 账号密码错误；4007 → 服务不可用。
    if (code == 2001 || code == 2004 || code == 2005) {
        showError(QString::fromUtf8("手机号或密码错误"));
    } else if (code == 4007) {
        showError(QString::fromUtf8("服务暂不可用，请稍后重试"));
    } else {
        showError(QString::fromUtf8("登录失败(code=%1)").arg(code));
    }
}

void LoginDialog::onRequestFailed(quint16 type, const QString& reason) {
    Q_UNUSED(type);
    setBusy(false);
    showError(QString::fromUtf8("连接失败，请检查网络（%1）").arg(reason));
}

}  // namespace ui
}  // namespace cm
