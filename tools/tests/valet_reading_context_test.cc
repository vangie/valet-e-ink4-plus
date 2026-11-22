#include "valet_reading_context.h"
#include <cassert>
#include <iostream>
int main() {
    using namespace valet;
    SelectStandaloneContext(); assert(!SelectedReadingContext().Valid()); assert(!SelectBookContext());
    PublishReadingPage("/sdcard/a.epub", "三国演义", "第一回", 2, "宴桃园豪杰三结义");
    assert(SelectBookContext()); auto first = SelectedReadingContext();
    assert(first.Valid() && first.page == 2 && first.title == "三国演义" && first.book_id.size() == 32);
    const auto key = ContextStorageKey("home");
    PublishReadingPage("/sdcard/a.epub", "三国演义", "第二回", 3, "下一页");
    assert(SelectedReadingContext().page == 2); // selected immutable snapshot stays fixed while recording
    assert(SelectBookContext() && SelectedReadingContext().page == 3);
    assert(ContextStorageKey("home") == key); // same book, same conversation
    assert(ContextStorageKey("work") != key);
    SelectStandaloneContext(); assert(!SelectedReadingContext().Valid()); assert(ContextStorageKey("home") != key);
    PublishReadingPage("/sdcard/b.epub", "三国演义", "第一回", 1, "另一个文件");
    assert(SelectBookContext() && ContextStorageKey("home") != key); // equal titles don't collide
    std::string text; for (int i=0;i<5000;++i) text += "汉";
    PublishReadingPage("/sdcard/c.epub", "字库", "正文", 1, text);
    assert(SelectBookContext()); auto clipped=SelectedReadingContext();
    assert(clipped.truncated && clipped.text.size() <= 12000 && clipped.text.size()%3==0);
    auto* json=ContextJson(clipped); assert(cJSON_IsObject(json)); cJSON_Delete(json);
    PublishReadingPage("/sdcard/image.cbz", "漫画", "", 1, ""); assert(!SelectBookContext());
    // 行拼回段落：段内直接接，段间换行，英文折行补空格
    using L = std::vector<PageLine>;
    assert(JoinPageLines(L{{"豪杰蹉跎", true}, {"运未通。", false}, {"林冲道：", true}}) == "豪杰蹉跎运未通。\n林冲道：");
    assert(JoinPageLines(L{{"a quick", true}, {"brown fox,", false}, {"jumps", false}}) == "a quick brown fox, jumps");
    // 首尾段落都被翻页截断：从前一页、后一页补全，标出本页
    const L prev{{"前一段。", true}, {"柴进道：“既是兄长要行，", true}};
    const L page{{"小人有个去处。”", false}, {"柴进道：“第三个唤做云里金刚宋", true}};
    const L next{{"万。”", false}, {"林冲听了。", true}};
    bool cut = false;
    const auto wide = ComposeReadingText(prev, page, next, 12000, cut);
    assert(!cut && wide.find("柴进道：“既是兄长要行，【本页开始】小人有个去处") != std::string::npos);
    assert(wide.find("云里金刚宋【本页结束】万。”") != std::string::npos);
    assert(wide.find("前一段") == std::string::npos && wide.find("林冲听了") == std::string::npos);   // 只补被截断的那一段
    // 首尾都是整段：原样
    assert(ComposeReadingText(prev, L{{"整段。", true}}, L{{"下一段。", true}}, 12000, cut) == "整段。" && !cut);
    // 超长段落：前一页整页都在段中，只留靠近本页的 600 字节以内，在句子边界开始，前面标「……」
    std::string longp; for (int i = 0; i < 400; ++i) longp += "很长的一句。";
    const auto lp = ComposeReadingText(L{{longp, false}}, L{{"接着。", false}}, L{}, 12000, cut, 600);
    const auto at = lp.find("【本页开始】", lp.find('\n'));   // 第一行是说明
    assert(at != std::string::npos && lp.find("……很长的一句。") != std::string::npos);
    assert(at - lp.find("……") <= 600 + 6);
    // 总长守住上限，当前页放不下才截并标记
    std::string big; for (int i = 0; i < 4500; ++i) big += "汉";
    const auto capped = ComposeReadingText(prev, L{{big, false}}, next, 12000, cut);
    assert(cut && capped.size() <= 12000 && capped.find("【本页结束】") != std::string::npos);
    // 选中「这一页」时才补全；页码对不上就用原来的本页文字
    PublishReadingPage("/sdcard/d.epub", "水浒传", "第十一回", 649, "本页");
    SetReadingTextExpander([] { return std::pair<int, std::string>{649, "补全后的正文"}; });
    assert(SelectBookContext() && SelectedReadingContext().text == "补全后的正文" && SelectedReadingContext().expanded);
    PublishReadingPage("/sdcard/d.epub", "水浒传", "第十一回", 650, "下一页");
    assert(SelectBookContext() && SelectedReadingContext().text == "下一页");   // 补全器还停在 649
    SetReadingTextExpander(nullptr);
    std::cout << "Reading contexts: rendered snapshots, book/assistant isolation, standalone clearing, UTF8 bounds, paragraph completion passed\n";
}
