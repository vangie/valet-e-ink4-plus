#pragma once
// 带成员短码的自建书库：同步阅读进度 + 想读标记。
// 从 /opds/<短码>/... 下的书记下进度地址 /api/u/<短码>/books/<id>/progress，
// 打开时问一次读到哪里（别处读得更新就跳过去），读的时候定时、离开时写回。
// 只走普通 OPDS 的书（没有短码）不同步。网络和 SD 都在后台任务里做，不卡 UI。
#include <functional>
#include <string>

namespace valet::sync {

/** @brief 从下载地址推出进度地址；不是带短码的同步书库地址时返回空串 */
std::string ProgressUrlFromDownload(const std::string& download_url);

/** @brief 记下这本书的进度地址并写 .sync sidecar（下载成功后调用，可在任意任务） */
void Remember(const std::string& book_path, const std::string& progress_url);

/** @brief 书库页打开一本已下好的书前调用：还没有同步记录时，用这个地址补上 */
void HintBeforeOpen(const std::string& book_path, const std::string& progress_url);

/** @brief 打开一本书后调用（LVGL 任务）：有进度地址就去问，别处读得更新时跳过去 */
void PullOnOpen(const std::string& book_path, int local_x10);

struct Position {
    int x10 = 0;           // 整本进度 0–1000
    int spine = -1;        // EPUB 第几节（从 0 开始）；不是 EPUB 时 -1
    int spine_x1000 = 0;   // 节内进度 0–1000
    const char* format = "epub";
};

/** @brief 写回当前位置（LVGL 任务）；和上次写的一样就不发 */
void Push(const std::string& book_path, const Position& pos);

/**
 * @brief 切换「想读」标记（后台任务）。
 * @param progress_url  /api/u/<key>/books/<id>/progress 形式的进度地址；wish 地址从这里推出
 * @param on            true = 加想读，false = 取消
 * @param callback      LVGL 任务里回调（ok = 请求成功，wish = 服务器返回的新状态）
 */
void ToggleWish(const std::string& progress_url, bool on,
                std::function<void(bool ok, bool new_wish)> callback);

} // namespace valet::sync
