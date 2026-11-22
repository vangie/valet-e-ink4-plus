// 书内搜索纯逻辑的主机测试：clang++ -std=c++17 -I main main/valet_book_search.cc tools/tests/valet_book_search_test.cc
#include "valet_book_search.h"
#include <cassert>
#include <iostream>
using namespace valet::booksearch;
int main() {
    assert(FormatPosition(15, 32) == "c15.p32");
    auto p = ParsePosition("c15.p32"); assert(p.Valid() && p.chapter == 15 && p.paragraph == 32);
    p = ParsePosition("3.0"); assert(p.Valid() && p.chapter == 3 && p.paragraph == 0);
    for (const char* bad : {"", "c", "c1", "c1.", "c-1.p2", "c1.p2x", "x1.p2"}) assert(!ParsePosition(bad).Valid());
    assert(NormalizeQuery("  Song WAN \n") == "song wan");
    assert(NormalizeQuery("\xe3\x80\x80宋万") == "宋万");   // 全角空格
    assert(NormalizeQuery("   ").empty() && NormalizeQuery(std::string(200, 'a')).empty());
    assert(Find("云里金刚宋万。", "宋万") == std::string("云里金刚").size());
    assert(Find("Lin Chong met SONG Wan", "song wan") == 14);
    assert(Find("abc", "abcd") == std::string::npos && Find("宋", "") == std::string::npos);
    // 不从字中间匹配：「万」的后两个字节不能和别的字拼出命中
    assert(Find("万", "\x87") == std::string::npos);
    // 摘录：短段原样；长段以命中为中心、字符边界、两头加省略号
    assert(Excerpt("短段落", 0, 3) == "短段落");
    std::string longp; for (int i = 0; i < 300; ++i) longp += "汉"; longp += "宋万"; for (int i = 0; i < 300; ++i) longp += "汉";
    const auto ex = Excerpt(longp, longp.find("宋万"), 6, 120);
    assert(ex.find("宋万") != std::string::npos && ex.size() <= 120 + 6 && ex.rfind("…", 0) == 0 && ex.size() >= 6);
    assert(ex.compare(ex.size() - 3, 3, "…") == 0);
    assert(ClipUtf8("汉汉", 4) == "汉");
    // 段落拼接：续接片段接上；章末收尾
    ParagraphJoiner j;
    assert(!j.Feed("第一段", false));
    assert(!j.Feed("的后半", true));
    assert(j.Feed("第二段", false) && j.Finished() == "第一段的后半" && j.FinishedIndex() == 0);
    assert(j.Flush() && j.Finished() == "第二段" && j.FinishedIndex() == 1);
    assert(!j.Flush());
    // 当前页在哪一段：页首行可能在段中间，空白不计
    assert(ParagraphHasAnchor("柴进道：“既是兄长要行，小人有个去处。作书一封与兄长去，如何？”", "小人有个去处。作书一封与兄长去，"));
    assert(ParagraphHasAnchor("a  b c d e f g h", "abcdefg"));
    assert(!ParagraphHasAnchor("任何一段", "如何"));   // 太短不认
    std::cout << "Book search: positions, queries, matching, excerpts, paragraphs, anchors passed\n";
}
