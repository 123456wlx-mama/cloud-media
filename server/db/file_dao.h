#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cm/base/error_code.h"

namespace cm {
namespace db {

class MysqlConn;

struct BlobRow {
    std::uint64_t fid;
    std::string md5;
    std::uint64_t size;
    std::string path;
    std::uint32_t refCount;
    std::uint8_t state;

    BlobRow();
};

struct UserFileRow {
    std::uint64_t ufId;
    std::uint64_t uid;
    std::uint64_t parentId;
    std::uint64_t fid;  // 目录为 0
    std::string name;
    bool isDir;
    std::string deletedAt;  // 等于 activeDeletedAt() 表示未删除
    std::string createdAt;  // DATETIME(6) 文本，可原样作为游标回传
    std::string updatedAt;

    UserFileRow();
};

// insertBlobOnDuplicateIncrement 的结局：
// Inserted（affected_rows=1，新建 blob）或 RefIncremented（affected_rows=2，命中已有 blob 并 +1）。
enum class BlobWriteKind {
    Inserted = 0,
    RefIncremented = 1
};

enum class FileListSort {
    NameAsc = 0,
    CreatedDesc = 1,
    UpdatedDesc = 2
};

// keyset 游标。NameAsc 时 cursorKey 是上一页末行的 uf_name；
// CreatedDesc/UpdatedDesc 时是上一页末行的 DATETIME(6) 文本（直接取 UserFileRow 的字段即可）。
// cursorKey 为空且 cursorId 为 0 表示首页。
struct FileListQuery {
    std::uint64_t uid;
    std::uint64_t parentId;
    FileListSort sort;
    std::size_t limit;
    std::string cursorKey;
    std::uint64_t cursorId;

    FileListQuery();
};

struct FileListPage {
    std::vector<UserFileRow> rows;
    bool hasMore;
    std::string nextKey;
    std::uint64_t nextId;

    FileListPage();
};

struct RestoreOutcome {
    std::uint64_t fid;
    std::string name;  // 冲突自动改名后的最终名
    bool alreadyRestored;

    RestoreOutcome();
};

// t_file（blob）与 t_user_file（用户节点）的 DAO。
// 不持有连接池，调用方用 MysqlPool::acquire 取得连接后构造。
// 所有 SQL 均为预处理语句；本类不拼接用户输入。
class FileDao {
public:
    explicit FileDao(MysqlConn& conn);

    FileDao(const FileDao&) = delete;
    FileDao& operator=(const FileDao&) = delete;

    // uf_deleted_at 的“未删除”哨兵值（DATETIME(6) 文本）。
    static const char* activeDeletedAt();

    ErrorCode findBlobByMd5(const std::string& md5, BlobRow* out);
    ErrorCode getBlobById(std::uint64_t fid, BlobRow* out);

    // INSERT ... ON DUPLICATE KEY UPDATE：命中 uk_f_md5 时只把 f_ref_count +1，不覆盖路径与状态。
    ErrorCode insertBlobOnDuplicateIncrement(const std::string& md5,
                                             std::uint64_t size,
                                             const std::string& path,
                                             std::uint64_t* fid,
                                             BlobWriteKind* kind);

    // 事务内尝试原名与 (2)…(99)；唯一键冲突继续下一个候选名，超限返回 AlreadyExists(1003)。
    // isDir=true 时 f_id 写 NULL（目录不持有 blob），fid 必须为 0。
    ErrorCode createUserFileAutoRename(std::uint64_t uid,
                                       std::uint64_t parentId,
                                       std::uint64_t fid,
                                       bool isDir,
                                       const std::string& name,
                                       std::uint64_t* ufId,
                                       std::string* finalName);

    // 秒传命中落库（06 §5.2 步骤 4），单事务：
    //  1) 同目录同名同 blob 的活跃节点已存在 → 直接复用，ref_count 不变（reused=true）；
    //  2) 否则 INSERT t_file ... ON DUPLICATE +1，再插入 t_user_file（名称冲突自动改名）。
    ErrorCode materializeInstantUpload(std::uint64_t uid,
                                       std::uint64_t parentId,
                                       const std::string& md5,
                                       std::uint64_t size,
                                       const std::string& name,
                                       std::uint64_t* fileId,
                                       std::uint64_t* ufId,
                                       std::string* finalName,
                                       bool* reused);

    // 含已软删节点（由调用方按 deletedAt 判断）。
    ErrorCode findUserFile(std::uint64_t uid, std::uint64_t ufId, UserFileRow* out);

    // 只列未删除节点；取 limit+1 行判断 has_more。
    ErrorCode listByParent(const FileListQuery& query, FileListPage* page);
    ErrorCode countByParent(std::uint64_t uid, std::uint64_t parentId, std::uint64_t* count);

    // 同一事务内：置 uf_deleted_at、文件节点条件减引用计数、归零置 f_state=2。
    // 已删除时 alreadyDeleted=true 且不再减计数（幂等）。
    ErrorCode softDelete(std::uint64_t uid, std::uint64_t ufId, bool* alreadyDeleted);

    // 同一事务内：恢复哨兵、修正 parent/name（冲突自动改名）、文件节点 +1、f_state 2→1。
    // 已恢复时 alreadyRestored=true 且不再加计数（幂等）。
    ErrorCode restore(std::uint64_t uid,
                      std::uint64_t ufId,
                      std::uint64_t parentId,
                      const std::string& name,
                      RestoreOutcome* out);

    // 物理删除单个节点行。目录子树由服务层按批次调用本方法。
    // 从未软删过的文件节点在此补减一次计数；已软删的不再减（避免无符号下溢）。
    ErrorCode purgeNode(std::uint64_t uid, std::uint64_t ufId, bool* deleted);

private:
    // 不含事务：调用方负责 BEGIN/COMMIT（createUserFileAutoRename 与秒传落库复用）。
    ErrorCode insertNodeWithRename(std::uint64_t uid,
                                   std::uint64_t parentId,
                                   std::uint64_t fid,
                                   bool isDir,
                                   const std::string& name,
                                   std::uint64_t* ufId,
                                   std::string* finalName);

    MysqlConn& m_conn;
};

}  // namespace db
}  // namespace cm
