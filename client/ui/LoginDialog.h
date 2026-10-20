#pragma once

#include <QDialog>

class QLineEdit;
class QLabel;
class QPushButton;

namespace cm {
namespace ui {

// 登录对话框骨架：只做输入校验与「通过」，不发网络请求（P8-1 接 LoginRq）。
// 视觉规范见 docs/specs/08-界面设计契约.md §4、状态见 §3（未登录/失败重试）。
class LoginDialog : public QDialog {
    Q_OBJECT

public:
    explicit LoginDialog(QWidget* parent = nullptr);

    QString tel() const;
    QString password() const;

private slots:
    void onLoginClicked();

private:
    QLineEdit* m_tel;
    QLineEdit* m_password;
    QLabel* m_error;
    QPushButton* m_loginButton;
};

}  // namespace ui
}  // namespace cm
