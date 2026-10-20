#pragma once

#include <QMainWindow>
#include <QString>

class QSplitter;

namespace cm {
namespace ui {

// 三段式主窗口：文件区 / 传输区 / 播放区。
// P8-0 只有骨架与空态；P8-2 起接 Model/View、传输队列、播放器。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    // 一个分区：标题栏（标题 + 右侧操作槽）+ 空态占位。
    QWidget* buildPane(const QString& title, const QString& hint,
                       const QString& objectName);
    QWidget* buildFilePane();
    QWidget* buildTransferPane();
    QWidget* buildPlayerPane();

    QSplitter* m_splitter;
};

}  // namespace ui
}  // namespace cm
