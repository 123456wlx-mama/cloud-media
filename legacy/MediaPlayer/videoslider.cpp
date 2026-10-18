#include "videoslider.h"

VideoSlider::VideoSlider(QWidget *parent)
    : QSlider(parent)
{
}

VideoSlider::~VideoSlider()
{
}

void VideoSlider::mousePressEvent(QMouseEvent *event)
{
    //把点击位置换算成滑块的值
    int value = QStyle::sliderValueFromPosition(
                minimum(), maximum(), event->pos().x(), width());
    setValue(value);
    emit SIG_valueChanged(value);
}
