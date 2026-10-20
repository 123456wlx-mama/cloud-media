#pragma once

#include <QMainWindow>
#include <QString>
#include <QVariantList>

class QSplitter;
class QTableView;
class QLabel;

namespace cm {

namespace netapi {
class NetClient;
}

namespace ui {

class FileTableModel;

// 三段式主窗口：文件区 / 传输区 / 播放区。
// P8-1：文件区接真实 ListFileRs；传输区与播放区仍是占位（P8-3 / P8-5）。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(netapi::NetClient* net, QWidget* parent = nullptr);

public slots:
    void refreshList();

private slots:
    void onLoginSucceeded(quint32 code, quint64 uid, const QString& name);
    void onLinkState(int state, const QString& detail);
    void onListFinished(quint32 code, const QVariantList& rows);
    void onRequestFailed(quint16 type, const QString& reason);

private:
    QWidget* buildPane(const QString& title, const QString& hint,
                       const QString& objectName);
    QWidget* buildFilePane();
    QWidget* buildTransferPane();
    QWidget* buildPlayerPane();
    void setLinkBadge(int state, const QString& detail);

    netapi::NetClient* m_net;
    QSplitter* m_splitter;
    FileTableModel* m_model;
    QTableView* m_table;
    QLabel* m_fileEmpty;
    QLabel* m_linkBadge;
    QLabel* m_listHint;
};

}  // namespace ui
}  // namespace cm
