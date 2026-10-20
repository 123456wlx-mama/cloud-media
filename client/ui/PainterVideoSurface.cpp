#include "ui/PainterVideoSurface.h"

#include <QPainter>

#include "ui/Letterbox.h"

namespace cm {
namespace ui {

PainterVideoSurface::PainterVideoSurface(QWidget* parent) : QWidget(parent) {
    setObjectName(QString::fromUtf8("videoCanvas"));
    setMinimumSize(160, 90);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
}

void PainterVideoSurface::setImage(const QImage& image) {
    if (image.isNull()) {
        return;
    }
    m_image = image;
    update();
}

void PainterVideoSurface::clear() {
    m_image = QImage();
    update();
}

void PainterVideoSurface::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0x12, 0x16, 0x1B));  // bg-base
    if (m_image.isNull()) {
        return;
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(letterboxRect(m_image.size(), size()), m_image);
}

}  // namespace ui
}  // namespace cm
