#include "ui/EmptyState.h"

#include <QFont>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QVector>

namespace cm {
namespace ui {

EmptyState::EmptyState(const QString& title, const QString& subtitle,
                       QWidget* parent)
    : QWidget(parent), m_title(title), m_subtitle(subtitle) {
    // 空态只是提示，别把播放区的鼠标事件吃掉
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

void EmptyState::setTexts(const QString& title, const QString& subtitle) {
    m_title = title;
    m_subtitle = subtitle;
    update();
}

void EmptyState::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 1) 虚线圆角框（留边，避免贴死控件边界）
    const QRectF frame = QRectF(rect()).adjusted(20.0, 16.0, -20.0, -16.0);
    if (frame.width() > 80.0 && frame.height() > 80.0) {
        QPen pen(QColor("#2A3341"));
        pen.setWidthF(1.5);
        pen.setStyle(Qt::DashLine);
        pen.setDashPattern(QVector<qreal>() << 6.0 << 5.0);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(frame, 14.0, 14.0);
    }

    // 2) 云朵（三个圆 + 一个圆角底，合并成一个路径）
    const qreal cloudW = 110.0;
    const qreal cloudH = 64.0;
    const qreal cx = static_cast<qreal>(width()) / 2.0;
    const qreal titleH = 26.0;
    const qreal subH = 22.0;
    const qreal blockH = cloudH + 18.0 + titleH + 6.0 + subH;
    const qreal top = (static_cast<qreal>(height()) - blockH) / 2.0;

    if (top > frame.top() + 4.0 && cloudW < frame.width()) {
        QPainterPath cloud;
        cloud.addEllipse(QPointF(cx - 28.0, top + 42.0), 22.0, 22.0);
        cloud.addEllipse(QPointF(cx + 2.0, top + 30.0), 30.0, 30.0);
        cloud.addEllipse(QPointF(cx + 32.0, top + 44.0), 20.0, 20.0);
        cloud.addRoundedRect(QRectF(cx - 50.0, top + 42.0, 100.0, 24.0), 12.0,
                             12.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#39434F"));
        painter.drawPath(cloud.simplified());
    }

    // 3) 两行文案
    QFont textFont = font();
    textFont.setPixelSize(20);
    painter.setFont(textFont);
    painter.setPen(QColor("#8A94A6"));
    const QRect titleRect(0, static_cast<int>(top + cloudH + 18.0), width(),
                          static_cast<int>(titleH));
    painter.drawText(titleRect, Qt::AlignHCenter | Qt::AlignTop, m_title);

    textFont.setPixelSize(16);
    painter.setFont(textFont);
    painter.setPen(QColor("#5E6B7A"));
    const QRect subRect(0, titleRect.bottom() + 6, width(),
                        static_cast<int>(subH));
    painter.drawText(subRect, Qt::AlignHCenter | Qt::AlignTop, m_subtitle);
}

}  // namespace ui
}  // namespace cm
