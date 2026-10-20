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
    if (!forcePainter && openGlAvailable()) {
        return new GlVideoSurface(parent);
    }
    return new PainterVideoSurface(parent);
}

}  // namespace ui
}  // namespace cm
