#include "ui/FileTableModel.h"

#include <QDateTime>
#include <QVariantMap>

namespace cm {
namespace ui {
namespace {

QString formatSize(quint64 bytes) {
    if (bytes < 1024ULL) {
        return QString::fromUtf8("%1 B").arg(bytes);
    }
    if (bytes < 1024ULL * 1024ULL) {
        return QString::fromUtf8("%1 KB").arg(bytes / 1024ULL);
    }
    if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        return QString::fromUtf8("%1 MB").arg(
            QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 1));
    }
    return QString::fromUtf8("%1 GB").arg(
        QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 'f', 2));
}

QString formatTime(quint64 ms) {
    if (ms == 0) {
        return QString::fromUtf8("—");
    }
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ms))
        .toString(QString::fromUtf8("yyyy-MM-dd HH:mm"));
}

}  // namespace

FileTableModel::FileTableModel(QObject* parent) : QAbstractTableModel(parent) {}

int FileTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return m_rows.size();
}

int FileTableModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return ColCount;
}

QVariant FileTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return QVariant();
    }
    const Row& row = m_rows[index.row()];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case ColName:
                return row.name;
            case ColSize:
                // 目录：直接子文件字节和（空目录显示 —）
                if (row.isDir) {
                    return row.size > 0 ? formatSize(row.size)
                                        : QString::fromUtf8("—");
                }
                return formatSize(row.size);
            case ColKind:
                if (row.isDir) {
                    return row.childCount > 0
                               ? QString::fromUtf8("文件夹 · %1 项").arg(row.childCount)
                               : QString::fromUtf8("文件夹");
                }
                return QString::fromUtf8("文件");
            case ColUpdatedAt:
                return formatTime(row.updatedAtMs);
            default:
                break;
        }
    }
    if (role == Qt::TextAlignmentRole && index.column() == ColSize) {
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (role == Qt::UserRole) {
        return QVariant::fromValue<qulonglong>(row.ufId);
    }
    return QVariant();
}

QVariant FileTableModel::headerData(int section, Qt::Orientation orientation,
                                    int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QVariant();
    }
    switch (section) {
        case ColName:
            return QString::fromUtf8("名称");
        case ColSize:
            return QString::fromUtf8("大小");
        case ColKind:
            return QString::fromUtf8("类型");
        case ColUpdatedAt:
            return QString::fromUtf8("修改时间");
        default:
            return QVariant();
    }
}

void FileTableModel::setRows(const QVariantList& rows) {
    beginResetModel();
    m_rows.clear();
    endResetModel();
    appendRows(rows);
}

// 分页追加：只插入新区间，不 reset 整表（契约 §1.3 不整表刷新）。
void FileTableModel::appendRows(const QVariantList& rows) {
    if (rows.isEmpty()) {
        return;
    }
    const int first = m_rows.size();
    beginInsertRows(QModelIndex(), first, first + rows.size() - 1);
    m_rows.reserve(first + rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const QVariantMap map = rows.at(i).toMap();
        Row row;
        row.ufId = map.value(QString::fromUtf8("ufId")).toULongLong();
        row.name = map.value(QString::fromUtf8("name")).toString();
        row.isDir = map.value(QString::fromUtf8("isDir")).toBool();
        row.size = map.value(QString::fromUtf8("size")).toULongLong();
        row.updatedAtMs = map.value(QString::fromUtf8("updatedAtMs")).toULongLong();
        row.childCount = map.value(QString::fromUtf8("childCount")).toUInt();
        m_rows.append(row);
    }
    endInsertRows();
}

void FileTableModel::clearRows() {
    if (m_rows.isEmpty()) {
        return;
    }
    beginResetModel();
    m_rows.clear();
    endResetModel();
}

bool FileTableModel::isDirAt(int row) const {
    if (row < 0 || row >= m_rows.size()) {
        return false;
    }
    return m_rows[row].isDir;
}

QString FileTableModel::nameAt(int row) const {
    if (row < 0 || row >= m_rows.size()) {
        return QString();
    }
    return m_rows[row].name;
}

quint64 FileTableModel::sizeAt(int row) const {
    if (row < 0 || row >= m_rows.size()) {
        return 0;
    }
    return m_rows[row].size;
}

quint64 FileTableModel::ufIdAt(int row) const {
    if (row < 0 || row >= m_rows.size()) {
        return 0;
    }
    return m_rows[row].ufId;
}

}  // namespace ui
}  // namespace cm
