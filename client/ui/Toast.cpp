#include "ui/Toast.h"

#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QTimer>

namespace cm {
namespace ui {

Toast::Toast(QWidget* parent)
    : QFrame(parent), m_text(nullptr), m_action(nullptr), m_timer(nullptr),
      m_effect(nullptr) {
    setObjectName(QString::fromUtf8("toast"));
    setAttribute(Qt::WA_StyledBackground, true);
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 10, 12, 10);
    layout->setSpacing(12);

    m_text = new QLabel(this);
    m_text->setObjectName(QString::fromUtf8("toastText"));
    layout->addWidget(m_text, 1);

    m_action = new QPushButton(this);
    m_action->setObjectName(QString::fromUtf8("toastAction"));
    m_action->setVisible(false);
    layout->addWidget(m_action);
    connect(m_action, SIGNAL(clicked()), this, SIGNAL(actionTriggered()));
    connect(m_action, &QPushButton::clicked, this, &Toast::hide);

    m_effect = new QGraphicsOpacityEffect(this);
    m_effect->setOpacity(1.0);
    setGraphicsEffect(m_effect);

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(3000);
    connect(m_timer, SIGNAL(timeout()), this, SLOT(startFade()));

    hide();
}

void Toast::showText(const QString& text, const QString& actionText) {
    m_text->setText(text);
    m_action->setText(actionText);
    m_action->setVisible(!actionText.isEmpty());
    m_effect->setOpacity(1.0);
    adjustSize();
    place();
    show();
    raise();
    m_timer->start();
}

void Toast::place() {
    QWidget* host = parentWidget();
    if (host == nullptr) {
        return;
    }
    const int x = (host->width() - width()) / 2;
    const int y = host->height() - height() - 24;
    move(x > 0 ? x : 0, y > 0 ? y : 0);
}

void Toast::startFade() {
    QPropertyAnimation* fade = new QPropertyAnimation(m_effect, "opacity", this);
    fade->setDuration(260);
    fade->setStartValue(1.0);
    fade->setEndValue(0.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);
    connect(fade, &QPropertyAnimation::finished, this, &Toast::hide);
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

}  // namespace ui
}  // namespace cm
