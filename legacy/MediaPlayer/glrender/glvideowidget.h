#ifndef GLVIDEOWIDGET_H
#define GLVIDEOWIDGET_H

// 自定义 OpenGL 视频显示控件(替代原来的 QLabel + QImage::scaled 显示)
// 为什么要写它:
//   1) Qt 自带的 scaled(KeepAspectRatio) 是 CPU 软件缩放, 默认取"最快"模式, 放大后画质糊、有毛边
//   2) 交给 OpenGL 后由显卡(GPU)做纹理采样缩放, 画质更好、更流畅, 还省 CPU
//   3) QLabel 还有个毛病: 窗口拉大后无法再缩小, 换成 QWidget 派生类后布局可自由伸缩
//
// 用法: 解码线程拿到一帧 QImage -> 信号槽抛给界面 -> 调用 setImage() -> 内部触发 paintGL() 绘制

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QImage>

class GLVideoWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    explicit GLVideoWidget(QWidget *parent = nullptr);
    ~GLVideoWidget();

public slots:
    /// 收到解码出来的一帧图像(跨线程信号槽传过来的, 已深拷贝)
    void setImage(const QImage &img);
    /// 清屏(停止播放/播放结束时显示黑屏)
    void clearShow();

protected:
    void initializeGL() override;          // 初始化 OpenGL 环境: 着色器程序 + 纹理
    void paintGL() override;               // 真正的绘制(把纹理画到控件上)
    void resizeGL(int w, int h) override;  // 控件大小改变

private:
    void buildShader();                    // 编译/链接着色器程序
    void uploadTexture();                  // 把当前帧上传到纹理
    void updateVertex();                   // 按控件尺寸等比缩放并居中, 更新顶点缓冲

private:
    QImage m_image;      // 当前要显示的帧(统一转成 RGBA8888 后再上传纹理)
    GLuint m_texture;    // 纹理对象
    GLuint m_program;    // 着色器程序
    GLuint m_vbo;        // 顶点缓冲: 每个顶点 4 个 float(x, y, u, v)
    int    m_locPos;     // 顶点坐标属性 location
    int    m_locTex;     // 纹理坐标属性 location
    int    m_locSampler; // 采样器 uniform location
    bool   m_ready;      // OpenGL 资源是否已初始化完成
};

#endif // GLVIDEOWIDGET_H
