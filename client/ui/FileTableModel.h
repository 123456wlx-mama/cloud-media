#pragma once

#include <QAbstractTableModel>
#include <QString>
#include <QVariantList>
#include <QVector>

namespace cm {
namespace ui {

// 文件区最小模型：名称 / 大小 / 类型 / 修改时间。
// 数据只由 netapi 的 listFinished 灌入，界面不自己造行（契约 §1.3）。
class FileTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { ColName = 0, ColSize, ColKind, ColUpdatedAt, ColCount };

    explicit FileTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void setRows(const QVariantList& rows);
    quint64 ufIdAt(int row) const;

private:
    struct Row {
        quint64 ufId;
        QString name;
        bool isDir;
        quint64 size;
        quint64 updatedAtMs;

        Row() : ufId(0), isDir(false), size(0), updatedAtMs(0) {}
    };

    QVector<Row> m_rows;
};

}  // namespace ui
}  // namespace cm
