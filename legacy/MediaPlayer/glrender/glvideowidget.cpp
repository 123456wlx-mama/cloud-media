#include "glvideowidget.h"

#include <QDebug>
#include <algorithm>

// 顶点着色器: 直接把归一化坐标(-1~1)输出, 纹理坐标传给片段着色器
static const char *VERT_SHADER =
        "attribute vec2 aPos;\n"
        "attribute vec2 aTex;\n"
        "varying vec2 vTex;\n"
        "void main() {\n"
        "    vTex = aTex;\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";

// 片段着色器: 采样纹理
// (GL_ES 下 float 没有默认精度, 必须声明; 桌面 OpenGL 不认 precision, 所以用宏隔开)
static const char *FRAG_SHADER =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "uniform sampler2D uTex;\n"
        "varying vec2 vTex;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(uTex, vTex);\n"
        "}\n";

GLVideoWidget::GLVideoWidget(QWidget *parent)
    : QOpenGLWidget(parent)
    , m_texture(0)
    , m_program(0)
    , m_vbo(0)
    , m_locPos(-1)
    , m_locTex(-1)
    , m_locSampler(-1)
    , m_ready(false)
{
    // 允许窗口自由伸缩(QLabel 只能变大不能变小)
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(160, 120);
}

GLVideoWidget::~GLVideoWidget()
{
    makeCurrent(); // 析构时也要保证有当前上下文, 否则 glDelete* 无效
    if( m_vbo )     glDeleteBuffers(1, &m_vbo);
    if( m_program ) glDeleteProgram(m_program);
    if( m_texture ) glDeleteTextures(1, &m_texture);
    doneCurrent();
}

void GLVideoWidget::initializeGL()
{
    // 用 QOpenGLFunctions 动态解析 GL 函数(因此不需要额外链接 opengl32)
    initializeOpenGLFunctions();

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // 背景黑色

    // 生成并配置纹理: 线性过滤 = 缩放更平滑(这正是我们想要的画质)
    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    buildShader();

    glGenBuffers(1, &m_vbo);
    m_ready = true;
}

void GLVideoWidget::buildShader()
{
    auto compile = [&](GLenum type, const char *src) -> GLuint {
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if( !ok )
        {
            char log[512] = {0};
            glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
            qDebug() << "GLVideoWidget shader compile error:" << log;
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    };

    GLuint vs = compile(GL_VERTEX_SHADER, VERT_SHADER);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FRAG_SHADER);
    if( !vs || !fs ) { m_ready = false; return; }

    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glLinkProgram(m_program);
    GLint ok = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if( !ok )
    {
        char log[512] = {0};
        glGetProgramInfoLog(m_program, sizeof(log), nullptr, log);
        qDebug() << "GLVideoWidget program link error:" << log;
    }
    // 着色器链接完就可以删掉(程序里已保留编译结果)
    glDeleteShader(vs);
    glDeleteShader(fs);

    m_locPos     = glGetAttribLocation(m_program, "aPos");
    m_locTex     = glGetAttribLocation(m_program, "aTex");
    m_locSampler = glGetUniformLocation(m_program, "uTex");
}

void GLVideoWidget::uploadTexture()
{
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 m_image.width(), m_image.height(), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, m_image.constBits());
}

void GLVideoWidget::updateVertex()
{
    // 等比缩放: 取宽高比例较大的那个做基准, 保证画面不变形且完整显示在控件内
    float winW = (float)width();
    float winH = (float)height();
    float imgW = (float)m_image.width();
    float imgH = (float)m_image.height();
    if( winW <= 0 || winH <= 0 || imgW <= 0 || imgH <= 0 ) return;

    float scale = std::max(imgW / winW, imgH / winH);
    // 归一化坐标: 控件范围是 -1~1, 所以"半宽"就是 显示宽度/控件宽度
    float halfW = (imgW / scale) / winW;
    float halfH = (imgH / scale) / winH;

    // 4 个顶点(三角形带), 每个顶点: x, y, u, v
    // QImage 第 1 行是图像顶部, 而纹理坐标 v=0 对应第 1 行, 所以底部用 v=1(否则画面上下颠倒)
    const float vert[] = {
        -halfW, -halfH,  0.0f, 1.0f,   // 左下
         halfW, -halfH,  1.0f, 1.0f,   // 右下
        -halfW,  halfH,  0.0f, 0.0f,   // 左上
         halfW,  halfH,  1.0f, 0.0f    // 右上
    };

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vert), vert, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(m_locPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glVertexAttribPointer(m_locTex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(m_locPos);
    glEnableVertexAttribArray(m_locTex);
}

void GLVideoWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);
    if( !m_ready || m_image.isNull() ) return; // 没有帧就只留黑屏

    glUseProgram(m_program);
    uploadTexture();
    updateVertex();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glUniform1i(m_locSampler, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glUseProgram(0);
}

void GLVideoWidget::resizeGL(int w, int h)
{
    Q_UNUSED(w)
    Q_UNUSED(h)
    if( !m_ready ) return;
    glViewport(0, 0, w, h);
    updateVertex(); // 尺寸变了要重新算居中位置
}

void GLVideoWidget::setImage(const QImage &img)
{
    if( img.isNull() ) { clearShow(); return; }

    // 纹理用 RGBA 字节序, 解码出来的是 RGB32, 这里统一转换一次并缓存, 绘制时就不用再转
    if( img.format() == QImage::Format_RGBA8888 )
        m_image = img;
    else
        m_image = img.convertToFormat(QImage::Format_RGBA8888);

    update(); // 触发 paintGL(在 GUI 线程执行, 跨线程安全)
}

void GLVideoWidget::clearShow()
{
    m_image = QImage(); // 置空, paintGL 只清屏 -> 显示黑屏
    update();
}
