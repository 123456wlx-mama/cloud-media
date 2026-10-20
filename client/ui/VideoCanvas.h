#pragma once

#include <QImage>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>

class QOpenGLShaderProgram;

namespace cm {
namespace ui {

// 视频画布：QOpenGLWidget + 纹理上屏，保持原始宽高比（黑边填充）。
// 只接收 QImage，不认识 FFmpeg/SDL（依赖红线）。
class VideoCanvas : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    explicit VideoCanvas(QWidget* parent = nullptr);
    ~VideoCanvas() override;

    void setImage(const QImage& image);
    void clear();

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
