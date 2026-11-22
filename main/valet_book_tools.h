#pragma once
// 给助理用的书内工具：在正在读的这本书里搜词、按位置读一段。
// 网关在助理调用 valet_book_search / valet_book_read 时发 valet.book.request 给设备，设备在后台任务里
// 另开一份文档按章节读（不碰阅读器正在用的会话），读完把结果交回协议（valet.book.result）。
// 书的内容只在被问到时、只按请求的范围离开设备。位置格式见 valet_book_search.h。
#include <functional>
#include <string>
#include <cJSON.h>

namespace valet {

// 阅读器每画一页报一次：哪本书（路径和格式只留在设备上）、在文档的第几章、这一页的第一行。
struct BookSource {
    std::string path, book_id, anchor;
    int format = 0;     // reader::BookFormat
    int chapter = -1;   // 文档里的章节序号（EPUB 阅读顺序 / EBOOK 章节表）；TXT 为 0
};
void PublishBookSource(const BookSource& source);
BookSource LatestBookSource();

// 跑一个请求（op = search / read，参数见网关的工具定义）。结果（成功或失败都是一个 JSON 对象，
// 归 reply 所有）在后台任务里交给 reply。已经有一个在跑时返回 false。
bool StartBookToolRequest(const BookSource& source, const cJSON* request, std::function<void(cJSON*)> reply);

}  // namespace valet
