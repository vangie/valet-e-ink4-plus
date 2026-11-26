#pragma once

#include <string>
#include <vector>

// 书库订阅（OPDS）：设备上存链接就够了，书名、进度都在书库那边。
// 配置页（/setup 的「书库订阅」卡片）和设备上的书架读的是同一份。
namespace valet {
inline constexpr size_t kMaxLibraries = 8;
inline constexpr size_t kMaxLibraryUrl = 512;

struct LibrarySource {
    std::string id;
    std::string name;    // 显示名，默认取链接的主机名
    std::string url;     // 订阅链接，http 或 https
    std::string member;  // 书斋这类带家人身份的订阅；链接里拿不到时留空
    bool rich = false;   // 带阅读进度/想读的订阅（书斋）；普通 OPDS 是 false
};

std::vector<LibrarySource> LoadLibrarySources();
/** @brief 保存整份列表；成功后后续读取立刻看到新值 */
bool SaveLibrarySources(const std::vector<LibrarySource>& items);

/** @brief 链接可用：http/https 开头、有主机、不太长 */
bool LibraryUrlValid(const std::string& url);
/** @brief 从链接推出显示名（主机名；书斋这类带短码的仍用主机名） */
std::string LibraryNameFromUrl(const std::string& url);
/** @brief 这条链接是不是带进度的订阅（书斋）：路径里有 /opds/<短码> 或 /api/u/<短码> */
bool LibraryRichUrl(const std::string& url);
/** @brief 新的订阅 id（随机） */
std::string NewLibraryId();
} // namespace valet
