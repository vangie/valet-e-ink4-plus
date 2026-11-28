#pragma once
// 微信读书（WeRead）核心协议与标准 EPUB 打包层：
// - 支持免登录（游客态）：全网搜书、读免费章节/试读章节并合成为本地标准 .epub
// - 支持扫码登录（账号态）：wr_vid/wr_skey/wr_rt 存 NVS，wr_rt 自动续期，同步云端书架与已购章节
// - 支持阅读进度与时长双向同步（对接 valet::sync）

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "opds_client.h"
#include "reader/download_gate.h"

namespace valet::weread {

struct BookItem {
    std::string book_id;
    std::string title;
    std::string author;
    std::string cover;
    std::string summary;
    std::string format;
    int progress_x10 = -1;       // 0..1000
    std::string chapter_uid;
    int chapter_offset = 0;
    long long update_time = 0;
};

struct ChapterItem {
    std::string uid;
    std::string title;
    int index = 0;
    int level = 1;
    int word_count = 0;
    bool paid = false;
};

struct RemoteProgress {
    int progress_x10 = -1;       // 0..1000
    std::string chapter_uid;
    int chapter_offset = 0;
    long long update_time = 0;
    int reading_time_sec = 0;
};

enum class QrState : uint8_t {
    kPending = 0,
    kComplete,
    kNeedOtp,
    kOtpMismatch,
    kExpired,
    kError,
};

/** @brief 是否已登录微信读书（NVS 有 vid 与 skey/rt） */
bool IsLoggedIn();
/** @brief 当前登录的微信读书 vid（未登录返回空串） */
std::string LoggedVid();
/** @brief 退出登录（清空 NVS 中的微信读书凭据） */
void Logout();

/** @brief 获取扫码登录 UID 与二维码 URL (https://weread.qq.com/web/confirm?uid=...) */
bool BeginQrLogin(std::string& uid, std::string& qr_url, std::string& err);
/** @brief 轮询扫码结果；成功时自动将 Cookie 持久化到 NVS */
bool PollQrLogin(const std::string& uid, const std::string& otp, QrState& state, std::string& err);

/** @brief 全网搜索微信读书（免登录/已登录均可），结果直接转为 OPDS Entry 供书库页统一展示 */
bool SearchBooks(const std::string& query, std::vector<valet::opds::Entry>& out, std::string& err);

/** @brief 拉取个人微信读书云书架（需登录；过期时自动用 wr_rt 续期一次），结果转为 OPDS Entry */
bool FetchShelf(std::vector<valet::opds::Entry>& out, std::string& err);

/**
 * @brief 下载并合成标准 EPUB 到 save_path（含旁路封面与 .sync 映射文件）
 * @param book_id       微信读书 bookId
 * @param fallback_title 标题兜底
 * @param fallback_author 作者兜底
 * @param cover_url     封面图 URL
 * @param save_path     目标 .epub 绝对路径
 * @param err_out       失败原因
 * @param on_progress   进度回调 (0..100)
 * @param progress_user 进度回调透传指针
 * @param gate          取消闸门
 * @param downloaded_chapters_out 成功下载的可读章节数
 * @param total_chapters_out      全书总章节数
 */
bool DownloadBookAsEpub(const std::string& book_id,
                        const std::string& fallback_title,
                        const std::string& fallback_author,
                        const std::string& cover_url,
                        const std::string& save_path,
                        std::string& err_out,
                        void (*on_progress)(int pct, void* user) = nullptr,
                        void* progress_user = nullptr,
                        DownloadGate* gate = nullptr,
                        int* downloaded_chapters_out = nullptr,
                        int* total_chapters_out = nullptr);

/** @brief 拉取单本书的云端阅读进度（需登录） */
bool FetchProgress(const std::string& book_id, RemoteProgress& out, std::string& err);

/** @brief 上传单本书的阅读进度到微信读书云端（需登录） */
bool UploadProgress(const std::string& book_id,
                    const std::string& chapter_uid,
                    int chapter_idx,
                    int chapter_offset,
                    int progress_x10,
                    int read_seconds,
                    std::string& err);

/** @brief 判断一本书（本地路径）是否来自微信读书（检查 .sync 是否为 weread://<bookId>） */
bool IsWeReadBook(const std::string& book_path, std::string* book_id_out = nullptr);

/** @brief 根据本地书籍路径和 EPUB spine 下标反查对应的微信读书 chapterUid；未找到返回空串 */
std::string ChapterUidForSpine(const std::string& book_path, int spine_index);

/** @brief 根据本地书籍路径和微信读书 chapterUid 反查对应的 EPUB spine 下标；未找到返回 -1 */
int SpineForChapterUid(const std::string& book_path, const std::string& chapter_uid);

/** @brief 获取本地微信读书 EPUB 已下载的章节数（读取 .wrmap 行数） */
int DownloadedChapterCount(const std::string& book_path);

}  // namespace valet::weread
