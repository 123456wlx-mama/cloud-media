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

// 打开分享成功后的二选一：保存到我的网盘（转存）/ 下载到本地。
// 两者语义完全不同：转存只加引用、不搬字节，节点进自己的目录树；
// 下载把文件取到本机「下载」文件夹，云端不产生任何记录。
class ShareReceiveDialog : public QDialog {
    Q_OBJECT

public:
    enum Choice { None = 0, Save, Download };

    explicit ShareReceiveDialog(const shareapi::ShareInfo& info,
                                QWidget* parent = nullptr);

    Choice choice() const { return m_choice; }

private:
    Choice m_choice;
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
