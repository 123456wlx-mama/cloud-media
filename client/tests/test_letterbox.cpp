// 渲染层纯函数单测：letterbox/pillarbox 目标矩形。
#include <gtest/gtest.h>

#include <QRect>
#include <QSize>

#include "ui/Letterbox.h"

TEST(Letterbox, WideImageInSquareLeavesTopBottomBars) {
    // 16:9 放进正方形：宽度撑满，上下黑边
    const QRect box = cm::ui::letterboxRect(QSize(640, 360), QSize(1000, 1000));
    EXPECT_EQ(QRect(0, 219, 1000, 562), box);
}

TEST(Letterbox, NarrowImageInWideFrameLeavesSideBars) {
    // 4:3 放进 2:1：高度撑满，左右黑边
    const QRect box = cm::ui::letterboxRect(QSize(640, 480), QSize(1000, 500));
    EXPECT_EQ(QRect(167, 0, 666, 500), box);
}

TEST(Letterbox, SameAspectFillsWholeTarget) {
    const QRect box = cm::ui::letterboxRect(QSize(1920, 1080), QSize(1920, 1080));
    EXPECT_EQ(QRect(0, 0, 1920, 1080), box);
    const QRect half = cm::ui::letterboxRect(QSize(1920, 1080), QSize(960, 540));
    EXPECT_EQ(QRect(0, 0, 960, 540), half);
}

TEST(Letterbox, PortraitVideoPillarboxed) {
    // 竖屏 1:2 放进正方形
    const QRect box = cm::ui::letterboxRect(QSize(300, 600), QSize(800, 800));
    EXPECT_EQ(QRect(200, 0, 400, 800), box);
}

TEST(Letterbox, TinyTargetKeepsAtLeastOnePixel) {
    const QRect box = cm::ui::letterboxRect(QSize(640, 360), QSize(100, 100));
    EXPECT_EQ(QRect(0, 22, 100, 56), box);
}

TEST(Letterbox, InvalidInputsReturnEmptyRect) {
    EXPECT_FALSE(cm::ui::letterboxRect(QSize(0, 0), QSize(100, 100)).isValid());
    EXPECT_FALSE(cm::ui::letterboxRect(QSize(640, 360), QSize(0, 100)).isValid());
    EXPECT_FALSE(cm::ui::letterboxRect(QSize(640, 0), QSize(100, 100)).isValid());
}
