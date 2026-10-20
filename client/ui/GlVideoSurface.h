#pragma once

#include <QImage>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>

#include "ui/VideoSurface.h"

class QOpenGLShaderProgram;

namespace cm {
namespace ui {

// 默认后端：QOpenGLWidget + 纹理上屏，保持宽高比（黑边填充）。
class GlVideoSurface : public QOpenGLWidget, protected QOpenGLFunctions,
                       public VideoSurface {
    Q_OBJECT

public:
    explicit GlVideoSurface(QWidget* parent = nullptr);
    ~GlVideoSurface() override;

    QWidget* widget() override { return this; }
    void setImage(const QImage& image) override;
    void clear() override;
    QString backendName() const override { return QString::fromUtf8("gl"); }

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    QOpenGLShaderProgram* m_program;
    QOpenGLBuffer m_vbo;
    GLuint m_texture;
    QImage m_image;
    bool m_dirty;
};

}  // namespace ui
}  // namespace cm
