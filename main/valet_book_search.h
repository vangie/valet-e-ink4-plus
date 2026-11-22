#pragma once
// 书内搜索 / 读一段的纯逻辑（不碰文件、不碰 ESP），给助理的 valet_book_search / valet_book_read 工具用。
// 位置写成「c章.p段」：章 = 文档里的章节序号（EPUB 的阅读顺序、EBOOK 的章节表、TXT 恒为 0），
// 段 = 这一章里第几段（从 0 起，一个不是续接片段的正文块算一段）。和排版、字号无关，换了字号也不变。
#include <cstddef>
#include <string>
#include <vector>

namespace valet::booksearch {

constexpr size_t kMaxQuery = 120;       // 搜索词最长（字节）
constexpr size_t kExcerptMax = 360;     // 每条结果的摘录最长（字节）
constexpr size_t kSearchReplyMax = 6000;   // 一次搜索回给网关的摘录合计
constexpr size_t kReadReplyMax = 8000;     // 一次读回给网关的正文合计
constexpr int kMaxHits = 20, kDefaultHits = 8;
constexpr int kMaxBefore = 20, kMaxAfter = 40;

struct Position {
    int chapter = -1, paragraph = -1;
    bool Valid() const { return chapter >= 0 && paragraph >= 0; }
};
std::string FormatPosition(int chapter, int paragraph);
// 「c15.p32」；也认「15.32」。不合法返回无效位置。
Position ParsePosition(const std::string& text);

// 搜索词规整：去掉首尾空白，ASCII 转小写。超长或为空返回空串。
std::string NormalizeQuery(const std::string& query);
// 在一段里找规整后的搜索词（ASCII 不分大小写），返回字节位置；找不到返回 npos。
size_t Find(const std::string& paragraph, const std::string& normalized_query);
// 以命中处为中心截一段摘录（UTF-8 字符边界），两头截掉时加「…」。
std::string Excerpt(const std::string& paragraph, size_t at, size_t length, size_t max = kExcerptMax);
// 按 UTF-8 字符边界截到 max 字节以内。
std::string ClipUtf8(const std::string& text, size_t max);

// 一个章节的正文块逐个喂进来，拼成段落：续接片段接在上一段后面。
class ParagraphJoiner {
public:
    // 返回 true 表示上一段已经结束（可以取 Finished()）。
    bool Feed(const std::string& text, bool continuation);
    // 本章结束：把最后一段收尾。返回 true 表示有一段。
    bool Flush();
    const std::string& Finished() const { return done_; }
    int FinishedIndex() const { return done_index_; }
private:
    std::string current_, done_;
    int index_ = -1, done_index_ = -1;
    bool open_ = false;
};

// 「当前页」在这一章的哪一段：阅读器给出这一页第一行文字（anchor），在段落里找。
// 空白不计，只比前 kAnchorMatch 字节。
bool ParagraphHasAnchor(const std::string& paragraph, const std::string& anchor);

}  // namespace valet::booksearch
