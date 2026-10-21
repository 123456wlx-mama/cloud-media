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

// 独立注册表单：手机号 / 密码 / 确认密码 / 昵称。
// 注册成功 accept()，由 LoginDialog 把手机号密码回填到登录框（先注册再登录）。
class RegisterDialog : public QDialog {
    Q_OBJECT

public:
    explicit RegisterDialog(netapi::NetClient* net, QWidget* parent = nullptr);

    QString tel() const;
    QString password() const;

private slots:
    void onRegisterClicked();
    void onRegisterFinished(quint32 code, quint64 uid);
    void onRequestFailed(quint16 type, const QString& reason);

private:
    void setBusy(bool busy);
    void showError(const QString& text);

    netapi::NetClient* m_net;
    QLineEdit* m_tel;
    QLineEdit* m_password;
    QLineEdit* m_confirm;
    QLineEdit* m_name;
    QLabel* m_error;
    QPushButton* m_ok;
};

}  // namespace ui
}  // namespace cm
