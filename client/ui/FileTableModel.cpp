#include "ui/FileTableModel.h"

#include <QDateTime>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
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
    // 年份省掉：列表里最长的一列是修改时间，省 5 个字符能给名称列让出宽度
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ms))
        .toString(QString::fromUtf8("MM-dd HH:mm"));
}

// ---------- 文件类型图标（运行时绘制，不引入图片资源） ----------
enum class FileKind { Folder, Video, Audio, Image, Text, Other };

FileKind kindOf(const QString& name, bool isDir) {
    if (isDir) {
        return FileKind::Folder;
    }
    const QString s = QFileInfo(name).suffix().toLower();
    if (s == "mp4" || s == "mkv" || s == "avi" || s == "mov" || s == "flv" ||
        s == "ts" || s == "m4v" || s == "webm") {
        return FileKind::Video;
    }
    if (s == "mp3" || s == "flac" || s == "wav" || s == "aac" || s == "m4a" ||
        s == "ogg" || s == "wma") {
        return FileKind::Audio;
    }
    if (s == "png" || s == "jpg" || s == "jpeg" || s == "gif" || s == "bmp" ||
        s == "webp") {
        return FileKind::Image;
    }
    if (s == "txt" || s == "md" || s == "pdf" || s == "doc" || s == "docx" ||
        s == "xls" || s == "xlsx" || s == "ppt" || s == "pptx" || s == "csv") {
        return FileKind::Text;
    }
    return FileKind::Other;
}

// 16x16 圆角色块 + 白色符号：文件夹/视频/音频/图片/文档/其他
QIcon iconFor(FileKind kind) {
    QColor bg("#4A5563");
    QString glyph;
    switch (kind) {
        case FileKind::Folder:
            bg = QColor("#E0A33E");
            break;
        case FileKind::Video:
            bg = QColor("#3D7FE0");
            glyph = QString::fromUtf8("\xE2\x96\xB6");  // ▶
            break;
        case FileKind::Audio:
            bg = QColor("#35C46B");
            glyph = QString::fromUtf8("\xE2\x99\xAA");  // ♪
            break;
        case FileKind::Image:
            bg = QColor("#B45CD6");
            glyph = QString::fromUtf8("\xE2\x97\x89");  // ◉
            break;
        case FileKind::Text:
            bg = QColor("#8A94A6");
            glyph = QString::fromUtf8("\xE2\x96\xA4");  // ▤
            break;
        default:
            break;
    }

    QPixmap pm(20, 20);
    pm.fill(Qt::transparent);
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(bg);
    painter.drawRoundedRect(QRectF(0.5, 0.5, 19.0, 19.0), 5.0, 5.0);
    if (!glyph.isEmpty()) {
        QFont font = painter.font();
        font.setPixelSize(12);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(QColor("#FFFFFF"));
        painter.drawText(pm.rect(), Qt::AlignCenter, glyph);
    }
    painter.end();
    return QIcon(pm);
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
                // 项数挪到悬浮提示里：列表宽度有限，类型列越短，名称列越能显示全
                return row.isDir ? QString::fromUtf8("文件夹")
                                 : QString::fromUtf8("文件");
            case ColUpdatedAt:
                return formatTime(row.updatedAtMs);
            default:
                break;
        }
    }
    if (role == Qt::TextAlignmentRole && index.column() == ColSize) {
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    }
    // 左侧类型图标（文件夹/视频/音频/图片/文档/其他）
    if (role == Qt::DecorationRole && index.column() == ColName) {
        return iconFor(kindOf(row.name, row.isDir));
    }
    // 文件名被省略号截断时，悬浮显示完整名和详情
    if (role == Qt::ToolTipRole) {
        if (row.isDir) {
            return QString::fromUtf8("%1\n文件夹 · %2 项\n修改时间：%3")
                .arg(row.name)
                .arg(row.childCount)
                .arg(formatTime(row.updatedAtMs));
        }
        return QString::fromUtf8("%1\n大小：%2\n修改时间：%3")
            .arg(row.name, formatSize(row.size), formatTime(row.updatedAtMs));
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
