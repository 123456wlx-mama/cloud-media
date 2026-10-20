#pragma once

#include <QImage>
#include <QWidget>

#include "ui/VideoSurface.h"

namespace cm {
namespace ui {

// 回退后端：QWidget + QPainter 画 QImage（远程桌面/没有 OpenGL 时用）。
class PainterVideoSurface : public QWidget, public VideoSurface {
    Q_OBJECT

public:
    explicit PainterVideoSurface(QWidget* parent = nullptr);

    QWidget* widget() override { return this; }
    void setImage(const QImage& image) override;
    void clear() override;
    QString backendName() const override { return QString::fromUtf8("painter"); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage m_image;
};

}  // namespace ui
}  // namespace cm
