#pragma once

#include <string>
#include <vector>

// 书库订阅（OPDS）：设备上存链接与可选认证信息，书名、进度在书库服务端。
// 配置页（/setup 的「书库订阅」卡片）和设备上的书架读的是同一份。
namespace valet {
inline constexpr size_t kMaxLibraries = 8;
inline constexpr size_t kMaxLibraryUrl = 512;

struct LibrarySource {
    std::string id;
    std::string name;     // 显示名，默认取链接的主机名
    std::string url;      // 订阅链接，http 或 https
    std::string member;   // 带成员身份的订阅；链接里拿不到时留空
    std::string user;     // HTTP Basic 认证用户名（可选）
    std::string password; // HTTP Basic / 密码认证（可选，只写不回传明文）
    bool rich = false;    // 带阅读进度/想读同步的订阅；普通 OPDS 是 false
};

std::vector<LibrarySource> LoadLibrarySources();
/** @brief 保存整份列表；成功后后续读取立刻看到新值 */
bool SaveLibrarySources(const std::vector<LibrarySource>& items);

/** @brief 链接可用：http/https 开头、有主机、不太长 */
bool LibraryUrlValid(const std::string& url);
/** @brief 从链接推出显示名（主机名） */
std::string LibraryNameFromUrl(const std::string& url);
/** @brief 这条链接是不是带进度的订阅：路径里有 /opds/<短码> 或 /api/u/<短码> */
bool LibraryRichUrl(const std::string& url);
/** @brief 根据请求 URL 匹配已配置书库的 Authorization 头（如 "Basic ..."）；无认证时返回空串 */
std::string LibraryAuthHeaderForUrl(const std::string& url);
/** @brief 新的订阅 id（随机） */
std::string NewLibraryId();
} // namespace valet
