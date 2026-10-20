#pragma once

#include <QFrame>

class QLabel;
class QPushButton;
class QTimer;
class QGraphicsOpacityEffect;

namespace cm {
namespace ui {

// 轻提示：底部居中，3s 后淡出；可带一个动作按钮（如「撤销」）。
// 契约 §3：删除/秒传必须有显式提示，不能只改状态文字。
class Toast : public QFrame {
    Q_OBJECT

public:
    explicit Toast(QWidget* parent);

    void showText(const QString& text, const QString& actionText = QString());

signals:
    void actionTriggered();

private slots:
    void startFade();

private:
    void place();

    QLabel* m_text;
    QPushButton* m_action;
    QTimer* m_timer;
    QGraphicsOpacityEffect* m_effect;
};

}  // namespace ui
}  // namespace cm
