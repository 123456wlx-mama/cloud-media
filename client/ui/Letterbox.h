#pragma once

#include <QRect>
#include <QSize>

namespace cm {
namespace ui {

// 把 image 按原始宽高比放进 target 并居中，返回实际绘制矩形。
// 横向视频留上下黑边（letterbox），纵向视频留左右黑边（pillarbox）。
inline QRect letterboxRect(const QSize& image, const QSize& target) {
    if (image.width() <= 0 || image.height() <= 0 || target.width() <= 0 ||
        target.height() <= 0) {
        return QRect();
    }
    const double imageAspect =
        static_cast<double>(image.width()) / static_cast<double>(image.height());
    const double targetAspect =
        static_cast<double>(target.width()) / static_cast<double>(target.height());
    if (imageAspect > targetAspect) {
        int h = static_cast<int>(static_cast<double>(target.width()) / imageAspect);
        if (h < 1) {
            h = 1;
        }
        return QRect(0, (target.height() - h) / 2, target.width(), h);
    }
    int w = static_cast<int>(static_cast<double>(target.height()) * imageAspect);
    if (w < 1) {
        w = 1;
    }
    return QRect((target.width() - w) / 2, 0, w, target.height());
}

}  // namespace ui
}  // namespace cm
