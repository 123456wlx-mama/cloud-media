#include "ui/VideoCanvas.h"

#include <QOpenGLShaderProgram>

namespace cm {
namespace ui {
namespace {

const char* kVertexShader =
    "attribute vec2 aPos;\n"
    "attribute vec2 aTex;\n"
    "varying vec2 vTex;\n"
    "void main() {\n"
    "    vTex = aTex;\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

const char* kFragmentShader =
    "varying vec2 vTex;\n"
    "uniform sampler2D uTex;\n"
    "void main() { gl_FragColor = texture2D(uTex, vTex); }\n";

// x, y, u, v（QImage 左上角对应 v=1，需要翻转）
const float kQuad[] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
    1.0f,  -1.0f, 1.0f, 1.0f,
    -1.0f, 1.0f,  0.0f, 0.0f,
    1.0f,  1.0f,  1.0f, 0.0f,
};

}  // namespace

VideoCanvas::VideoCanvas(QWidget* parent)
    : QOpenGLWidget(parent), m_program(nullptr), m_texture(0), m_dirty(false) {
    setObjectName(QString::fromUtf8("videoCanvas"));
    setMinimumSize(160, 90);
}

VideoCanvas::~VideoCanvas() {
    if (m_texture != 0) {
        makeCurrent();
        glDeleteTextures(1, &m_texture);
        doneCurrent();
    }
}

void VideoCanvas::setImage(const QImage& image) {
    if (image.isNull()) {
        return;
    }
    m_image = image;
    m_dirty = true;
    update();
}

void VideoCanvas::clear() {
    m_image = QImage();
    m_dirty = true;
    update();
}

void VideoCanvas::initializeGL() {
    initializeOpenGLFunctions();
    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    m_program = new QOpenGLShaderProgram(this);
    m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    m_program->bindAttributeLocation("aPos", 0);
    m_program->bindAttributeLocation("aTex", 1);
    m_program->link();

    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(kQuad, static_cast<int>(sizeof(kQuad)));
    m_vbo.release();
}

void VideoCanvas::paintGL() {
    glClearColor(0.07f, 0.09f, 0.11f, 1.0f);  // bg-base #12161B
    glClear(GL_COLOR_BUFFER_BIT);
    if (m_image.isNull() || m_program == nullptr) {
        return;
    }
    if (m_dirty) {
        const QImage rgba = m_image.convertToFormat(QImage::Format_RGBA8888);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.width(), rgba.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
        m_dirty = false;
    }

    const double imgAspect = static_cast<double>(m_image.width()) /
                             static_cast<double>(m_image.height());
    const double widgetAspect = static_cast<double>(width()) /
                                static_cast<double>(height() > 0 ? height() : 1);
    int vx = 0;
    int vy = 0;
    int vw = width();
    int vh = height();
    if (imgAspect > widgetAspect) {
        vh = static_cast<int>(width() / imgAspect);
        vy = (height() - vh) / 2;
    } else {
        vw = static_cast<int>(height() * imgAspect);
        vx = (width() - vw) / 2;
    }
    glViewport(vx, vy, vw, vh);

    m_program->bind();
    m_vbo.bind();
    m_program->enableAttributeArray(0);
    m_program->enableAttributeArray(1);
    m_program->setAttributeBuffer(0, GL_FLOAT, 0, 2, 4 * sizeof(float));
    m_program->setAttributeBuffer(1, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    m_program->setUniformValue("uTex", 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_program->disableAttributeArray(0);
    m_program->disableAttributeArray(1);
    m_vbo.release();
    m_program->release();
}

}  // namespace ui
}  // namespace cm
