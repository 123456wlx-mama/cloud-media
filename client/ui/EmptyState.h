#pragma once

#include <QString>
#include <QWidget>

namespace cm {
namespace ui {

// 纯代码绘制的空状态：虚线圆角框 + 云朵 + 两行提示文案。
// 不依赖任何图片资源（仓库 qrc 里只有 app.qss）。
class EmptyState : public QWidget {
    Q_OBJECT

public:
    EmptyState(const QString& title, const QString& subtitle,
               QWidget* parent = nullptr);

    void setTexts(const QString& title, const QString& subtitle);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_title;
    QString m_subtitle;
};

}  // namespace ui
}  // namespace cm
