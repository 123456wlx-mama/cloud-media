#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
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
    // 仅 listByParent 通过 LEFT JOIN t_file 填充；其余查询保持 0 / 空。
    std::uint64_t size;
    std::string md5;

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

// 回收站列表项：deletedAtMs 由 SQL 内 UNIX_TIMESTAMP 计算，避免应用侧解析 UTC 文本。
struct RecycleNode {
    std::uint64_t ufId;
    std::uint64_t fid;  // 目录为 0
    std::string name;
    bool isDir;
    std::uint64_t parentId;
    std::uint64_t deletedAtMs;
    std::uint64_t size;  // 文件节点取 blob 大小；目录为 0

    RecycleNode();
};

struct RecycleListQuery {
    std::uint64_t uid;
    std::size_t limit;
    std::uint64_t cursorDeletedMs;  // 0 + cursorId==0 表示首页
    std::uint64_t cursorId;

    RecycleListQuery();
};

struct RecycleListPage {
    std::vector<RecycleNode> rows;
    bool hasMore;
    std::uint64_t nextDeletedMs;
    std::uint64_t nextId;

    RecycleListPage();
};

// 彻底删除一批节点的计数汇总。
struct PurgeBatchResult {
    std::uint32_t filesDeleted;
    std::uint32_t dirsDeleted;
    std::uint32_t blobsOrphaned;  // 本次减到 0 置孤儿的 blob 数

    PurgeBatchResult();
};

// 目录节点的直接子项统计（列表页一次查询覆盖所有目录行）。
struct ChildSummary {
    std::uint32_t childCount;
    std::uint64_t size;  // 直接子文件字节和；子目录不计（MySQL 5.7 无 CTE，避免递归）

    ChildSummary() : childCount(0), size(0) {}
};

// GC 候选：f_state=2、无任何用户节点（含回收站）、无有效分享、超过宽限期的 blob。
struct OrphanBlobRow {
    std::uint64_t fid;
    std::string md5;
    std::uint64_t size;
    std::string path;  // 相对 blob 根：aa/bb/<md5>

    OrphanBlobRow();
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

    // countByParent 超时（3024）时的返回值；与 wire 的“未知”哨兵一致。
    static const std::uint64_t kUnknownCount = 0xFFFFFFFFFFFFFFFFULL;

    ErrorCode findBlobByMd5(const std::string& md5, BlobRow* out);
    ErrorCode getBlobById(std::uint64_t fid, BlobRow* out);

    // 元数据说"就绪"但物理文件已经不在时调用：把 f_state 置 3（缺失）。
    // 之后秒传不再命中它，只有真实上传会把它改回 1；用于把"假就绪"暴露成状态。
    ErrorCode markBlobMissing(std::uint64_t fid);

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

    // 不含事务：调用方必须已 BEGIN（UploadComplete 的合并落库事务复用同一事务）。
    ErrorCode insertUserFileAutoRenameInTx(std::uint64_t uid,
                                           std::uint64_t parentId,
                                           std::uint64_t fid,
                                           const std::string& name,
                                           std::uint64_t* ufId,
                                           std::string* finalName);

    // 幂等回包：同用户同目录下指向该 blob 的活跃节点，取 uf_id 最大的一条。
    ErrorCode findActiveNodeByFid(std::uint64_t uid,
                                  std::uint64_t parentId,
                                  std::uint64_t fid,
                                  UserFileRow* out);

    // 跨目录可见性：该用户在任意目录下是否有指向 fid 的未删除节点（LIMIT 1）。
    // 用于播放进度上报校验“文件属于当前用户”，不关心所在目录。
    ErrorCode hasVisibleFileByFid(std::uint64_t uid,
                                  std::uint64_t fid,
                                  bool* visible);

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
    // LEFT JOIN t_file 回填 size / md5（目录 f_id 为 NULL，得 0 / 空串）。
    ErrorCode listByParent(const FileListQuery& query, FileListPage* page);
    // 孤儿 blob 扫描（GC 第一道筛选）：f_state=2、f_ref_count=0、
    // 最后更新（孤儿时刻，f_updated_at）超过 graceSeconds、
    // 不存在任何 t_user_file 节点（回收站里的也算，必须等彻底删除）、
    // 无未过期有效分享；按 f_id 升序 LIMIT ?。
    // 真正的删除前还会在事务内 FOR UPDATE 复核一次。
    ErrorCode listOrphanBlobs(std::uint64_t graceSeconds,
                              std::size_t limit,
                              std::vector<OrphanBlobRow>* out);

    // 批量统计目录的直接子项数与字节和（一次 IN 查询，按上页号分组）。
    ErrorCode summarizeChildren(std::uint64_t uid,
                                const std::vector<std::uint64_t>& parentIds,
                                std::map<std::uint64_t, ChildSummary>* out);

    // 首页 COUNT：200ms 上限 + LIMIT 10001 截断（最多返回 10001）。
    // 命中 max_execution_time（3024）时返回 Ok 且 *count = kUnknownCount，由调用方降级展示。
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

    // 回收站列表：排序 (uf_deleted_at_ms DESC, uf_id DESC)，keyset 游标 (ms, id)。
    // 用 UNIX_TIMESTAMP 把 DATETIME(6) 折算成毫秒，与 wire 的 cursor_deleted_ms 口径一致，
    // 避免「ms 游标 + 微秒 DATETIME 排序」丢行。
    ErrorCode listRecycle(const RecycleListQuery& query, RecycleListPage* page);

    // BFS 收集整棵子树（含软删后代），根在最前；超过 maxNodes 返回 TooManyNodes(3008)。
    // 用于彻底删除目录：先收集、判上限，再按批物理删除。
    ErrorCode listSubtree(std::uint64_t uid,
                          std::uint64_t rootUfId,
                          std::size_t maxNodes,
                          std::vector<UserFileRow>* out);

    // 在调用方事务内批量物理删除节点（彻底删除目录，200 行/事务）。
    // 每个节点：锁行 → DELETE → 活跃文件节点补减计数（已软删不二次减）。
    // 已不存在的节点跳过（重跑幂等）。
    ErrorCode purgeNodesInTx(std::uint64_t uid,
                             const std::vector<std::uint64_t>& ufIds,
                             PurgeBatchResult* out);

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
