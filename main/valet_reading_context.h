#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <cJSON.h>
namespace valet {
struct ReadingContext {
    std::string book_id, title, chapter, text;
    int page = 0;
    bool truncated = false;
    bool expanded = false;   // 已经补全了被页面截断的段落
    bool Valid() const { return !book_id.empty() && page > 0 && !text.empty(); }
};
// UI publishes an immutable copy of the page actually rendered; never reads SD.
// 选中「这一页」（SelectBookContext）时才请阅读器补全前后被截断的段落（见 SetReadingTextExpander）。
// 书的稳定编号（路径的 SHA-512 前 128 位，十六进制），助理那边按它分会话。
std::string BookIdForPath(const std::string& path);
void PublishReadingPage(const std::string& path, const std::string& title,
                        const std::string& chapter, int page, const std::string& text);
ReadingContext LatestReadingPage();

// 页面上的一行正文；para_start = 这一行是段首（缩进、段前空、章名）。
struct PageLine { std::string text; bool para_start = false; };
// 把一页的行拼回段落：段内的行直接接上（英文单词之间补空格），段与段之间换行。
std::string JoinPageLines(const std::vector<PageLine>& lines);
// 发给助理的正文：当前页 + 把被页面截断的首尾段落从前一页 / 后一页补全（各最多 side_max 字节，
// 尽量在句子边界截），并用【本页开始】【本页结束】标出这一页。总长不超过 max；当前页本身放不下时截断并置 truncated。
std::string ComposeReadingText(const std::vector<PageLine>& prev, const std::vector<PageLine>& page,
                               const std::vector<PageLine>& next, size_t max, bool& truncated, size_t side_max = 1500);
// 阅读器打开时注册：返回 {页码, 补全后的正文}；读不到返回 {0, ""}。只在 UI 任务、阅读器空闲时调用。
void SetReadingTextExpander(std::function<std::pair<int, std::string>()> expander);
bool SelectBookContext();
void SelectStandaloneContext();
ReadingContext SelectedReadingContext();
// 正文里长按选中的字：问的时候连同所在的句子一起带给助理，悬窗的提示也写着它。悬窗收起时清掉。
struct ReadingQuote {
    std::string text;       // 选中的字
    std::string sentence;   // 选中处所在的整句
    bool word = true;       // 一个词（四个字以内）；否则当一句
    bool Valid() const { return !text.empty(); }
};
void SetReadingQuote(const ReadingQuote& quote);
void ClearReadingQuote();
ReadingQuote CurrentReadingQuote();
// 带上选中的字时，跟问题一起发给助理的那句说明；没有选中返回空。说明以 kReadingQuoteNoteStart 开头（显示回显时去掉它）
inline constexpr const char* kReadingQuoteNoteStart = "（我在这一页选中了「";
std::string ReadingQuoteNote();
std::string ContextStorageKey(const std::string& assistant_id);
cJSON* ContextJson(const ReadingContext& context);
}
