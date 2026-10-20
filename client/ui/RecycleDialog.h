#pragma once

#include <QDialog>

class QLabel;
class QListWidget;

namespace cm {

namespace recycleapi {
class RecycleFlow;
}

namespace ui {

// 回收站：列表 + 恢复 / 彻底删除（只软删语义，无保留期）。
class RecycleDialog : public QDialog {
    Q_OBJECT

public:
    explicit RecycleDialog(recycleapi::RecycleFlow* flow, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void rebuild();

private:
    recycleapi::RecycleFlow* m_flow;
    QListWidget* m_list;
    QLabel* m_hint;
};

}  // namespace ui
}  // namespace cm
