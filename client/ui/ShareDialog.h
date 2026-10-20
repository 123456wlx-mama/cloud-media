#pragma once

#include <QDialog>

#include "shareapi/ShareFlow.h"

class QLabel;
class QLineEdit;

namespace cm {
namespace ui {

// 分享结果弹窗：分享码 + 提取码 + 过期时间，一键复制（契约 §2.1）。
class ShareDialog : public QDialog {
    Q_OBJECT

public:
    explicit ShareDialog(const shareapi::ShareInfo& info, QWidget* parent = nullptr);

signals:
    void cancelRequested(const QString& shareCode);

private:
    void copy(const QString& text, const QString& what);

    QString m_shareCode;
};

// 打开分享：分享码 + 提取码 → ShareOpenRq；2003/2004 的错误行就地显示。
class ShareOpenDialog : public QDialog {
    Q_OBJECT

public:
    explicit ShareOpenDialog(shareapi::ShareFlow* flow, QWidget* parent = nullptr);

protected:
    void accept() override;

private slots:
    void onOpened();
    void onFailed(quint32 code, const QString& text);

private:
    shareapi::ShareFlow* m_flow;
    QLineEdit* m_codeEdit;
    QLineEdit* m_accessEdit;
    QLabel* m_error;
};

}  // namespace ui
}  // namespace cm
