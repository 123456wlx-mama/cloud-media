#ifndef VIDEOSLIDER_H
#define VIDEOSLIDER_H

#include <QSlider>
#include <QMouseEvent>
#include <QStyle>

//自定义进度条: 重写鼠标点击事件, 支持点击任意位置直接跳转
class VideoSlider : public QSlider
{
    Q_OBJECT
public:
    explicit VideoSlider(QWidget *parent = 0);
    ~VideoSlider();

signals:
    void SIG_valueChanged(int value); //点击后发出的跳转信号

protected:
    void mousePressEvent(QMouseEvent *event);
};

#endif // VIDEOSLIDER_H
