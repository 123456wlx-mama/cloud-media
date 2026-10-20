#pragma once

#include <QImage>
#include <QString>

class QWidget;

namespace cm {
namespace ui {

// 渲染后端接口：只吃 QImage，不认识 FFmpeg/SDL（依赖红线）。
// GlVideoSurface 是默认实现；PainterVideoSurface 用于远程桌面/无 GL 环境回退。
class VideoSurface {
public:
    virtual ~VideoSurface() {}

    // 供布局使用的 QWidget（对象自身）。
    virtual QWidget* widget() = 0;
    virtual void setImage(const QImage& image) = 0;
    virtual void clear() = 0;
    // 日志用："gl" / "painter"
    virtual QString backendName() const = 0;
};

// forcePainter=true 或 OpenGL 上下文创建失败时返回 Painter 实现。
VideoSurface* createVideoSurface(QWidget* parent, bool forcePainter);
bool openGlAvailable();

}  // namespace ui
}  // namespace cm
