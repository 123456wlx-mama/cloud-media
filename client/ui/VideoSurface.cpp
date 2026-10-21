#include "ui/VideoSurface.h"

#include <QOpenGLContext>
#include <QSurfaceFormat>

#include "ui/GlVideoSurface.h"
#include "ui/PainterVideoSurface.h"

namespace cm {
namespace ui {

bool openGlAvailable() {
    QOpenGLContext context;
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    context.setFormat(format);
    return context.create();
}

VideoSurface* createVideoSurface(QWidget* parent, bool forcePainter) {
    VideoSurface* surface = nullptr;
    if (!forcePainter && openGlAvailable()) {
        surface = new GlVideoSurface(parent);
    } else {
        surface = new PainterVideoSurface(parent);
    }
    // 必须撑满播放区，否则按 sizeHint 缩在一角
    surface->widget()->setSizePolicy(QSizePolicy::Expanding,
                                     QSizePolicy::Expanding);
    return surface;
}

}  // namespace ui
}  // namespace cm
