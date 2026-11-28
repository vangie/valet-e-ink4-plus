#pragma once
// 在线书库（OPDS）：按订阅链接拉目录、搜索、看条目，条目里带封面和下载地址。
// 只做标准 OPDS（Atom + opds:acquisition），所以 Gutenberg、Calibre-Web 及自建书库都能用。
// 纯逻辑（解析、地址补全、选格式）不碰网络，可以在电脑上单测；拉取和下载在另一层。
#include <cstdint>
#include <string>
#include <vector>

namespace valet::opds {

constexpr size_t kMaxTitle = 300;
constexpr size_t kMaxAuthor = 160;
constexpr size_t kMaxSummary = 900;
constexpr size_t kMaxFeedBytes = 512 * 1024;   // 一页 feed 的上限，超过就当失败，避免吃光内存
constexpr int kMaxEntries = 60;                // 一页最多收这么多条

// 一个下载格式：epub / txt 优先，其它（mobi/azw3/pdf）记下来但设备打不开
struct Acquisition {
    std::string url;      // 补全成绝对地址
    std::string type;     // application/epub+zip 之类
    std::string ext;      // epub / txt / mobi / azw3 / pdf
    uint64_t length = 0;  // 0 = 不知道
};

struct Entry {
    std::string id;       // feed 里的 id，没有就用下载地址
    std::string title;
    std::string author;
    std::string summary;
    std::string cover;    // 封面绝对地址（可能为空）
    std::string thumbnail;// 缩略图绝对地址（可能为空）
    std::string nav;      // 没有下载格式时：点进去的下一级目录（绝对地址）
    std::string library;  // 来自哪个书库（订了多个时列表里标出来）
    bool wish = false;    // 服务端「想读」（通过 /api/u/<key>/books?status=wish 时填）
    float progress = 0;  // 服务端进度 0–1（书单里带回来的；打开时用 pull 再刷新）
    std::vector<Acquisition> files;
    // 设备能直接打开的最好格式；没有就是 nullptr
    const Acquisition* Best() const;
};

struct Feed {
    std::string title;              // 这一页的标题
    std::vector<Entry> entries;
    std::string next;               // 「下一页」绝对地址，没有就空
    std::string search_template;    // 带 {searchTerms} 的绝对地址，没有就空
    bool truncated = false;         // 这一页太大，只收了前面一部分
};

/** @brief 把 feed 里的相对地址补成绝对地址；base 是这次请求的地址 */
std::string ResolveUrl(const std::string& base, const std::string& href);
/** @brief 解析一页 feed；xml 不合法或不是 feed 时返回 false，feed 里能解析多少算多少 */
bool ParseFeed(const std::string& xml, const std::string& base, Feed& out);
/** @brief 用搜索词填搜索模板；模板为空或词为空时返回空串 */
std::string SearchUrl(const std::string& search_template, const std::string& query);
/** @brief 按扩展名认格式；认不出返回空串 */
std::string ExtFromType(const std::string& type, const std::string& href);
/** @brief 书名、作者、简介清一下（去首尾空白、压掉换行、截断到上限） */
std::string CleanText(const std::string& text, size_t max);

} // namespace valet::opds
