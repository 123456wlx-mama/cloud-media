#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;
class QLabel;
class QPushButton;

namespace cm {

namespace netapi {
class NetClient;
}

namespace ui {

// 登录对话框：真实发 LoginRq，按契约 §2.1 的错误码给文案。
class LoginDialog : public QDialog {
    Q_OBJECT

public:
    explicit LoginDialog(netapi::NetClient* net, QWidget* parent = nullptr);

    QString tel() const;
    QString password() const;
    // 登录成功后的账号 id；未成功时为 0。调用方用它隔离本地传输任务缓存。
    quint64 uid() const { return m_uid; }

private slots:
    void onLoginClicked();
    void onRegisterClicked();
    void onLoginFinished(quint32 code, quint64 uid, const QString& name);
    void onRequestFailed(quint16 type, const QString& reason);

private:
    void setBusy(bool busy);
    void showError(const QString& text);

    netapi::NetClient* m_net;
    QLineEdit* m_tel;
    QLineEdit* m_password;
    QLabel* m_error;
    QPushButton* m_loginButton;
    quint64 m_uid = 0;
};

}  // namespace ui
}  // namespace cm
