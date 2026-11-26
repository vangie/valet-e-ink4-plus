#pragma once
// 在线书库的网络层：拉一页 feed（文本）。下载书走 reader::DownloadCloudBook。
// 走和书下载同一条路（Board 的 Http），所以代理、局域网直连都自动生效。
#include <string>

#include "opds_client.h"

namespace valet::opds {

struct FetchResult {
    bool ok = false;
    int status = 0;       // HTTP 状态码；0 = 没连上
    std::string body;     // ok 时的 feed 文本
    bool truncated = false; // 太大，只收了前 kMaxFeedBytes
    std::string error;    // 失败原因（已本地化，可直接显示）
};

/** @brief 拉一页 feed；超过 kMaxFeedBytes 只留前面的（truncated），超时算失败 */
FetchResult FetchFeed(const std::string& url, int timeout_ms = 15000);

} // namespace valet::opds
